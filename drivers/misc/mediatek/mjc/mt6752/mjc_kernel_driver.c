#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/xlog.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>

#include <linux/earlysuspend.h>

#include <mach/irqs.h>
#include <asm/io.h>

#include <mach/m4u.h>
#include <mach/m4u_port.h>

#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>

#include <mach/sync_write.h>

#include "mach/mt_clkmgr.h"
#include "mjc_kernel_driver.h"
#include "mjc_kernel_compat_driver.h"


#define MJC_ASSERT(x) if(!(x)){xlog_printk(ANDROID_LOG_ERROR, "MJC", "assert fail, file:%s, line:%d", __FILE__, __LINE__);}

#define MTK_MJC_DBG
#ifdef MTK_MJC_DBG
#define MJCDBG(string, args...)	xlog_printk(ANDROID_LOG_DEBUG, "MJC", "[pid=%d]"string,current->tgid,##args);
#else
#define MJCDBG(string, args...)
#endif

#define MTK_MJC_MSG
#ifdef MTK_MJC_MSG
#define MJCMSG(string, args...)	xlog_printk(ANDROID_LOG_INFO, "MJC", "[pid=%d]"string,current->tgid,##args)
#else
#define MJCMSG(string, args...)
#endif

#define MJC_WriteReg32(addr,data)  mt_reg_sync_writel(data, addr)
#define MJC_Reg32(addr)             (*(volatile unsigned int *)(addr))

#define MJC_DEVNAME     "MJC"
#define MTK_MJC_DEV_MAJOR_NUMBER 168
#define MJC_FORCE_REG_NUM 100
#define EFUSE_MJC_IDX (3)
#define EFUSE_MJC_BIT (1<<16)

static DEFINE_SPINLOCK(ContextLock);
static DEFINE_SPINLOCK(HWLock);


static MJC_CONTEXT_T grContext;        
static MJC_CONTEXT_T grHWLockContext;  

static dev_t mjc_devno = MKDEV(MTK_MJC_DEV_MAJOR_NUMBER,0);
static struct cdev *g_mjc_cdev;
static struct class *pMjcClass = NULL;
static struct device* mjcDevice = NULL;
unsigned long gulRegister, gu1PaReg, gu1PaSize;
int gi4IrqID;
MJC_WRITE_REG_T gfWriteReg[MJC_FORCE_REG_NUM];


static int _mjc_CreateEvent(MJC_EVENT_T *a_prParam)
{
    wait_queue_head_t *prWaitQueue;
    unsigned char *pu1Flag;

    prWaitQueue = (wait_queue_head_t *)kmalloc(sizeof(wait_queue_head_t),GFP_ATOMIC);
    pu1Flag     = (unsigned char *)kmalloc(sizeof(unsigned char),GFP_ATOMIC);
    if(prWaitQueue != 0)
    {
        init_waitqueue_head(prWaitQueue);
        a_prParam->pvWaitQueue = (void *)prWaitQueue;
    }
    else
    {
        MJCMSG("[Error]_mjc_CreateEvent() Event wait Queue failed to create\n");
    }

    if(pu1Flag != 0)
    {
        a_prParam->pvFlag= (void *)pu1Flag;
        *((unsigned char *)a_prParam->pvFlag) = 0;
    }
    else
    {
        MJCMSG("[Error]_mjc_CreateEvent() Event flag failed to create\n");
    }

    return 0;
}


static int _mjc_CloseEvent(MJC_EVENT_T *a_prParam)
{
    wait_queue_head_t *prWaitQueue;
    unsigned char *pu1Flag;

    prWaitQueue = (wait_queue_head_t *)a_prParam->pvWaitQueue;
    pu1Flag     = (unsigned char *)a_prParam->pvFlag;

    if (prWaitQueue)
    {
        kfree(prWaitQueue);
    }
    if (pu1Flag)
    {
        kfree(pu1Flag);
    }

    a_prParam->pvWaitQueue = 0;
    a_prParam->pvFlag = 0;
    return 0;
}

static int _mjc_WaitEvent(MJC_EVENT_T *a_prParam)
{
    wait_queue_head_t *prWaitQueue;
    long               timeout_jiff;
    int i4Status;

    prWaitQueue   = (wait_queue_head_t *)a_prParam->pvWaitQueue;
    timeout_jiff = (a_prParam->u4TimeoutMs) * HZ / 1000;
    

    if (0 == wait_event_interruptible_timeout(*prWaitQueue, *((unsigned char *)a_prParam->pvFlag), timeout_jiff))
    {
        i4Status = 1;
    }
    else
    {
        i4Status = 0;
    }
    *((unsigned char *)a_prParam->pvFlag) = 0;

    return i4Status;
}

static int _mjc_SetEvent(MJC_EVENT_T *a_prParam)
{
    wait_queue_head_t *prWaitQueue;

    

    prWaitQueue = (wait_queue_head_t *)a_prParam->pvWaitQueue;

    if (a_prParam->pvFlag != NULL)
    {
        *((unsigned char *)a_prParam->pvFlag) = 1;
    }
    else
    {
        MJCMSG("[Error] _mjc_SetEvent() Event flag should not be null\n");
    }

    if (prWaitQueue != NULL)
    {
        wake_up_interruptible(prWaitQueue);
    }
    else
    {
        MJCMSG("[[Error] _mjc_SetEvent() Wait Queue should not be null\n");
    }
    return 0;
}

static void _mjc_m4uConfigPort(void)
{
    int u4Status;
    M4U_PORT_STRUCT rM4uPort;
    rM4uPort.Virtuality = 1;						   
    rM4uPort.Security = 0;
    rM4uPort.Distance = 0;
    rM4uPort.Direction = 0;
    rM4uPort.domain = 3;

	rM4uPort.ePortID = M4U_PORT_MJC_MV_RD;  
    u4Status = m4u_config_port(&rM4uPort);
    if (u4Status != 0)
    {
        MJCMSG("[ERROR] m4u_config_port(%d) fail!! status = %d\n", rM4uPort.ePortID, u4Status);
    }

	rM4uPort.ePortID = M4U_PORT_MJC_MV_WR;  
    u4Status = m4u_config_port(&rM4uPort);
    if (u4Status != 0)
    {
        MJCMSG("[ERROR] m4u_config_port(%d) fail!! status = %d\n", rM4uPort.ePortID, u4Status);
    }

	rM4uPort.ePortID = M4U_PORT_MJC_DMA_RD;  
    u4Status = m4u_config_port(&rM4uPort);
    if (u4Status != 0)
    {
        MJCMSG("[ERROR] m4u_config_port(%d) fail!! status = %d\n", rM4uPort.ePortID, u4Status);
    }

    rM4uPort.ePortID = M4U_PORT_MJC_DMA_WR;  
    u4Status = m4u_config_port(&rM4uPort);
    if (u4Status != 0)
    {
        MJCMSG("[ERROR] m4u_config_port(%d) fail!! status = %d\n", rM4uPort.ePortID, u4Status);
    }
}

m4u_callback_ret_t mjc_m4u_fault_callback(int port, unsigned int mva, void* data)
{
    printk("[ERROR] mjc faut call port=%d, mva=0x%x, data=%lx\n", port, mva, (long)data);

	printk("Sys Basic: 0x%x, Status: 0x%x, FS00: 0x%x, RDMA00: 0x%x, RDMA01: 0x%x\n", 
		   MJC_Reg32(gulRegister+0x4),
		   MJC_Reg32(gulRegister+0x70), MJC_Reg32(gulRegister+0xc00),
		   MJC_Reg32(gulRegister+0x100), MJC_Reg32(gulRegister+0x104) );

	printk("ACT FB Address (0x%x,0x%x,0x%x), (0x%x,0x%x,0x%x), (0x%x)\n",
           MJC_Reg32(gulRegister+0xc4c), MJC_Reg32(gulRegister+0xc50), MJC_Reg32(gulRegister+0xc54),
           MJC_Reg32(gulRegister+0xc58), MJC_Reg32(gulRegister+0xc5c), MJC_Reg32(gulRegister+0xc60),
           MJC_Reg32(gulRegister+0xc64) );
	printk("PLB FB Address (0x%x,0x%x,0x%x), (0x%x,0x%x,0x%x), (0x%x)\n",
           MJC_Reg32(gulRegister+0xc68), MJC_Reg32(gulRegister+0xc6c), MJC_Reg32(gulRegister+0xc70),
           MJC_Reg32(gulRegister+0xc74), MJC_Reg32(gulRegister+0xc78), MJC_Reg32(gulRegister+0xc7c),
           MJC_Reg32(gulRegister+0xc80) );

	printk("WD Address (0x%x,0x%x)\n",
		   MJC_Reg32(gulRegister+0xc3c), MJC_Reg32(gulRegister+0xc40) );

	printk("MV Address BI(0x%x,0x%x), TD(0x%x,0x%x), CUR(0x%x,0x%x), PV(0x%x)\n",
		   MJC_Reg32(gulRegister+0xc1c), MJC_Reg32(gulRegister+0xc20),
		   MJC_Reg32(gulRegister+0xc30), MJC_Reg32(gulRegister+0xc34),
		   MJC_Reg32(gulRegister+0xc24), MJC_Reg32(gulRegister+0xc28),
		   MJC_Reg32(gulRegister+0xc2c) );

	

	return M4U_CALLBACK_HANDLED;
}

extern u32 get_devinfo_with_index(u32 index);
static int mjc_open(struct inode *pInode, struct file *pFile)
{
    unsigned long ulFlags;
    unsigned int u4efuse;
#ifdef CONFIG_MTK_SEGMENT_TEST	
	unsigned int u4TestValue;
#endif
    
    MJCDBG("mjc_open() pid = %d\n", current->pid);

#ifndef CONFIG_MTK_SEGMENT_TEST	
    u4efuse = get_devinfo_with_index(EFUSE_MJC_IDX);
     
    if ((u4efuse & EFUSE_MJC_BIT) != 0)
    {
    	  MJCMSG("[ERROR] mjc efuse no support %d\n", u4efuse);
        return -1;
    }
#endif	

    
    enable_clock(MT_CG_DISP0_SMI_COMMON, "mjc");
	enable_clock(MT_CG_DISP0_LARB4_AXI_ASIF_MM, "mjc");
	enable_clock(MT_CG_DISP0_LARB4_AXI_ASIF_MJC, "mjc");
    enable_clock(MT_CG_MJC_SMI_LARB, "mjc");
	enable_clock(MT_CG_MJC_TOP_GROUP0, "mjc");
	enable_clock(MT_CG_MJC_TOP_GROUP1, "mjc");
	enable_clock(MT_CG_MJC_TOP_GROUP2, "mjc");
	enable_clock(MT_CG_MJC_LARB4_AXI_ASIF, "mjc");	

#ifdef CONFIG_MTK_SEGMENT_TEST
    u4TestValue = MJC_Reg32((gulRegister+4));
	MJCDBG("Pre-Read: 0x%x\n", u4TestValue);
	MJC_WriteReg32((gulRegister+4), 0x123456);
    MJC_WriteReg32((gulRegister+4), 0x123456);
	u4TestValue = MJC_Reg32((gulRegister+4));
	MJCDBG("Read: 0x%x\n", u4TestValue);
	if (u4TestValue == 0x123456)
	{
	    MJCMSG("[MJC efuse] HW enable\n");
	}
	else
	{
	    MJCMSG("[MJC efuse] HW disable\n");
		return -1;
	}
#endif	

	spin_lock_irqsave(&ContextLock, ulFlags);
	grContext.rEvent.u4TimeoutMs = 0xFFFFFFFF;
	spin_unlock_irqrestore(&ContextLock, ulFlags);

	spin_lock_irqsave(&HWLock, ulFlags);
	grHWLockContext.rEvent.u4TimeoutMs = 0xFFFFFFFF;
	spin_unlock_irqrestore(&HWLock, ulFlags);

	_mjc_m4uConfigPort();

	m4u_register_fault_callback(M4U_PORT_MJC_MV_RD, mjc_m4u_fault_callback, (void *)0);
	m4u_register_fault_callback(M4U_PORT_MJC_MV_WR, mjc_m4u_fault_callback, (void *)0);
	m4u_register_fault_callback(M4U_PORT_MJC_DMA_RD, mjc_m4u_fault_callback, (void *)0);
	m4u_register_fault_callback(M4U_PORT_MJC_DMA_WR, mjc_m4u_fault_callback, (void *)0);

	MJCDBG("Mapping register: 0x%lx, PA:0x%lx, Size:0x%lx, Irq ID:%d\n", gulRegister, gu1PaReg, gu1PaSize, gi4IrqID);

	return 0;
}

static int mjc_release(struct inode *pInode, struct file *pFile)
{
    MJCDBG("mjc_release() pid = %d\n", current->pid);

	m4u_unregister_fault_callback(M4U_PORT_MJC_MV_RD);
	m4u_unregister_fault_callback(M4U_PORT_MJC_MV_WR);
	m4u_unregister_fault_callback(M4U_PORT_MJC_DMA_RD);
	m4u_unregister_fault_callback(M4U_PORT_MJC_DMA_WR);

	
    disable_clock(MT_CG_MJC_SMI_LARB, "mjc");
	disable_clock(MT_CG_MJC_TOP_GROUP0, "mjc");
	disable_clock(MT_CG_MJC_TOP_GROUP1, "mjc");
	disable_clock(MT_CG_MJC_TOP_GROUP2, "mjc");
	disable_clock(MT_CG_DISP0_SMI_COMMON, "mjc");
	disable_clock(MT_CG_DISP0_LARB4_AXI_ASIF_MJC, "mjc");
	disable_clock(MT_CG_DISP0_LARB4_AXI_ASIF_MM, "mjc");	
	disable_clock(MT_CG_MJC_LARB4_AXI_ASIF, "mjc");

	return 0;
}

static long mjc_ioctl(struct file *pfile, unsigned int u4cmd, unsigned long u4arg)
{
    int ret = 0;
	int u4FirstUse;
	unsigned long ulAdd;
	unsigned long ulFlags;
	MJC_IOCTL_LOCK_HW_T rLockHW;
	MJC_IOCTL_ISR_T rIsr;
	MJC_READ_REG_T rReadReg;
	MJC_WRITE_REG_T rWriteReg;
	MJC_IOCTL_SRC_CLK_T rSrcClk;
	MJC_IOCTL_REG_INFO_T rRegInfo;
    int cnt = 0;

    MJCDBG("mjc_ioctl() command:%u\n", u4cmd);

	switch(u4cmd)
    {
        case MJC_LOCKHW:
		{
			MJCDBG("+ mjc_ioctl() MJC_LOCKHW + tid = %d\n", current->pid);

			if(copy_from_user(&rLockHW, (void __user *)u4arg, sizeof(MJC_IOCTL_LOCK_HW_T)))
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_LOCKHW copy_from_user fail\n");
				return -1;
			}

			if (sizeof(MJC_IOCTL_LOCK_HW_T) != rLockHW.u4StructSize)
			{
			   MJCMSG("[ERROR] mjc_ioctl() MJC_LOCKHW context size mismatch (user:%d, kernel:%ld)\n", rLockHW.u4StructSize, sizeof(MJC_IOCTL_LOCK_HW_T));
			   return -1;
			}

			spin_lock_irqsave(&HWLock, ulFlags);
	    	if(grHWLockContext.rEvent.u4TimeoutMs == 0xFFFFFFFF)
	    	{
	    	    grHWLockContext.rEvent.u4TimeoutMs = 1;
	    	    u4FirstUse = 1;
	    	}
			else
			{
			    u4FirstUse = 0;
			}
	        spin_unlock_irqrestore(&HWLock, ulFlags);

            
			if(u4FirstUse == 1)
			{
			    _mjc_WaitEvent(&(grHWLockContext.rEvent));
				spin_lock_irqsave(&HWLock, ulFlags);
				grHWLockContext.rEvent.u4TimeoutMs = 1000;
				spin_unlock_irqrestore(&HWLock, ulFlags);
			}
			else
			{
			    ret = _mjc_WaitEvent(&(grHWLockContext.rEvent));
			}
			

			if (ret == 1)
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_LOCKHW HW has been usaged\n");
			    return -1;
			}
            
			enable_irq(gi4IrqID);

            MJCDBG("- mjc_ioctl() MJC_LOCKHW + tid = %d\n", current->pid);
		}
		break;

        case MJC_WAITISR:
		{
			MJCDBG("mjc_ioctl() MJC_WAITISR + tid = %d\n", current->pid);

			if(copy_from_user(&rIsr, (void __user *)u4arg, sizeof(MJC_IOCTL_ISR_T)))
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_WAITISR copy_from_user fail\n");
				return -1;
			}

			if (sizeof(MJC_IOCTL_ISR_T) != rIsr.u4StructSize)
			{
			   MJCMSG("[ERROR] mjc_ioctl() MJC_WAITISR context size mismatch (user:%d, kernel:%ld)\n", rIsr.u4StructSize, sizeof(MJC_IOCTL_ISR_T));
			   return -1;
			}

			MJCDBG(" isrevent timeout setting (%x, %x)\n", grContext.rEvent.u4TimeoutMs, rIsr.u4TimeoutMs);
	    	spin_lock_irqsave(&ContextLock, ulFlags);
	    	grContext.rEvent.u4TimeoutMs = rIsr.u4TimeoutMs;
	        spin_unlock_irqrestore(&ContextLock, ulFlags);
			

	        ret = _mjc_WaitEvent(&(grContext.rEvent));
	        MJCDBG("mjc_ioctl() waitdone MJC_WAITISR TID:%d, ret:%d\n", current->pid, ret);

			if (ret != 0)
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_WAITISR TimeOut\n");		   			    
				spin_lock_irqsave(&HWLock, ulFlags);
	            _mjc_SetEvent(&(grHWLockContext.rEvent));
	            spin_unlock_irqrestore(&HWLock, ulFlags);

				disable_irq_nosync(gi4IrqID);  

				return -2;
			}
        }
		break;

		case MJC_READ_REG:
		{
			MJCDBG("mjc_ioctl() MJC_READ_REG + tid = %d\n", current->pid);

			if(copy_from_user(&rReadReg, (void __user *)u4arg, sizeof(MJC_READ_REG_T)))
            {
                MJCMSG("[ERROR] mjc_ioctl() MJC_READ_REG copy_from_user failed\n");
                return -1;
            }

            if(rReadReg.reg>(gu1PaReg+gu1PaSize)|| rReadReg.reg<gu1PaReg)
            {
                MJCMSG("[ERROR] mjc_ioctl() MJC_READ_REG, addr invalid, addr min=0x%lx, max=0x%lx, addr=0x%lx \n",
                    gu1PaReg,
                    (gu1PaReg+gu1PaSize),
                    rReadReg.reg);
                return -1;
            }

            ulAdd = gulRegister+(rReadReg.reg & 0xfff);
            rReadReg.val = MJC_Reg32(ulAdd) & rReadReg.mask;

            MJCDBG("read 0x%lx (0x%lx)= 0x%x (0x%x)\n", rReadReg.reg, ulAdd, rReadReg.val, rReadReg.mask);

            if(copy_to_user((void __user *)u4arg, &rReadReg, sizeof(MJC_READ_REG_T)))
            {
                MJCMSG("[ERROR] mjc_ioctl() MJC_READ_REG, copy_to_user failed\n");
                return -1;
            }
		}
		break;

		case MJC_WRITE_REG:
		{
			MJCDBG("mjc_ioctl() MJC_WRITE_REG + tid = %d\n", current->pid);

			if(copy_from_user(&rWriteReg, (void __user *)u4arg, sizeof(MJC_WRITE_REG_T)))
            {
                MJCMSG("[ERROR] mjc_ioctl() MJC_WRITE_REG copy_from_user failed\n");
                return -1;
            }

			MJCDBG("write  0x%lx = 0x%x (0x%x)\n", rWriteReg.reg, rWriteReg.val, rWriteReg.mask);

            if(rWriteReg.reg>(gu1PaReg+gu1PaSize) || rWriteReg.reg<gu1PaReg)
            {
                MJCMSG("[ERROR] mjc_ioctl() MJC_WRITE_REG, addr invalid, addr min=0x%lx, max=0x%lx, addr=0x%lx \n",
                    gu1PaReg,
                    (gu1PaReg+gu1PaSize),
                    rWriteReg.reg);
                return -1;
            }

            for(cnt=0;cnt<MJC_FORCE_REG_NUM;cnt++)
            {
                if(gfWriteReg[cnt].reg==0||gfWriteReg[cnt].reg==rWriteReg.reg)
                {
                    gfWriteReg[cnt].reg=rWriteReg.reg;
                    gfWriteReg[cnt].val=rWriteReg.val;
                    gfWriteReg[cnt].mask=rWriteReg.mask;
                    break;
                }
            }

            ulAdd = gulRegister+(rWriteReg.reg & 0xfff);
            MJC_WriteReg32(ulAdd, ((MJC_Reg32(ulAdd) & ~rWriteReg.mask) | (rWriteReg.val & rWriteReg.mask)));
		}
		break;

        case MJC_WRITE_REG_TBL:
        {
            MJCDBG("mjc_ioctl() MJC_WRITE_REG_TBL + tid = %d\n", current->pid);
            
            for(cnt=0;cnt<MJC_FORCE_REG_NUM;cnt++)
            {
                if(gfWriteReg[cnt].reg == 0) break;
                MJCMSG("s(%lx %x)\n", gfWriteReg[cnt].reg, gfWriteReg[cnt].val);                
                ulAdd = gulRegister+(gfWriteReg[cnt].reg & 0xfff);
                MJC_WriteReg32(ulAdd, ((MJC_Reg32(ulAdd) & ~gfWriteReg[cnt].mask) | (gfWriteReg[cnt].val & gfWriteReg[cnt].mask)));
            }
        }
        break;

        case MJC_CLEAR_REG_TBL:
        {
            MJCDBG("mjc_ioctl() MJC_CLEAR_REG_TBL + tid = %d\n", current->pid);
            for(cnt=0;cnt<MJC_FORCE_REG_NUM;cnt++)
            {
                gfWriteReg[cnt].reg = 0;
                gfWriteReg[cnt].val = 0;
                gfWriteReg[cnt].mask = 0;                 
            }
        }
        break;

		case MJC_SOURCE_CLK:
		{
			MJCDBG("mjc_ioctl() MJC_SOURCE_CLK + tid = %d\n", current->pid);

			if(copy_from_user(&rSrcClk, (void __user *)u4arg, sizeof(MJC_IOCTL_SRC_CLK_T)))
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_SOURCE_CLK copy_from_user fail\n");
				return -1;
			}

			if (sizeof(MJC_IOCTL_SRC_CLK_T) != rSrcClk.u4StructSize)
			{
			   MJCMSG("[ERROR] mjc_ioctl() MJC_SOURCE_CLK context size mismatch (user:%d, kernel:%ld)\n", rSrcClk.u4StructSize, sizeof(MJC_IOCTL_SRC_CLK_T));
			   return -1;
			}

            MJCDBG(" frame rate =%d\n", rSrcClk.u2OutputFramerate);
			if (rSrcClk.u2OutputFramerate == 600) 
			{
			    
			}
			else if (rSrcClk.u2OutputFramerate == 1200) 
			{
			    
			}
			else
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_SOURCE_CLK fail frame rate : %d\n", rSrcClk.u2OutputFramerate);
				return -1;
			}
		}
		break;

		case MJC_REG_INFO:
		{
			MJCDBG("mjc_ioctl() MJC_REG_INFO + tid = %d\n", current->pid);

			if(copy_from_user(&rRegInfo, (void __user *)u4arg, sizeof(MJC_IOCTL_REG_INFO_T)))
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_REG_INFO copy_from_user fail\n");
				return -1;
			}

			if (sizeof(MJC_IOCTL_REG_INFO_T) != rRegInfo.u4StructSize)
			{
			   MJCMSG("[ERROR] mjc_ioctl() MJC_REG_INFO context size mismatch (user:%d, kernel:%ld)\n", rRegInfo.u4StructSize, sizeof(MJC_IOCTL_REG_INFO_T));
			   return -1;
			}

			rRegInfo.ulRegPAddress = gu1PaReg;
			rRegInfo.ulRegPSize = gu1PaSize;

			MJCDBG(" PA = 0x%lx, PS = 0x%lx\n", rRegInfo.ulRegPAddress, rRegInfo.ulRegPSize);

			if(copy_to_user((void __user *)u4arg, &rRegInfo, sizeof(MJC_IOCTL_REG_INFO_T)))
			{
			    MJCMSG("[ERROR] mjc_ioctl() MJC_REG_INFO copy_to_user fail\n");
				return -1;
			}
		}
		break;

		default:
			MJCMSG("[ERROR] mjc_ioctl() No such command 0x%x!!\n", u4cmd);
			break;

	}
	return 0;
}

static int mjc_mmap(struct file *pFile, struct vm_area_struct *pVma)
{
    unsigned long ulLength;
	unsigned long ulAddr;
	
	MJCDBG("mjc_mmap \n");

	MJCMSG("start = 0x%lx, pgoff= 0x%lx vm_end= 0x%lx \n",
	       pVma->vm_start,
	       pVma->vm_pgoff,
	       pVma->vm_end);	

    pVma->vm_page_prot = pgprot_noncached(pVma->vm_page_prot);

	ulLength = pVma->vm_end - pVma->vm_start;
	ulAddr = pVma->vm_pgoff<<PAGE_SHIFT;
	
	if((ulLength > gu1PaSize) || (ulAddr < gu1PaReg) || (ulAddr > gu1PaReg+gu1PaSize))
	{
	    MJCMSG("[ERROR] mmap region error: Length(0x%lx), pfn(0x%lx)\n", ulLength, ulAddr);
		return -EAGAIN;
	}    

    if (remap_pfn_range(pVma, pVma->vm_start, pVma->vm_pgoff, ulLength, pVma->vm_page_prot))
    {
        MJCMSG("[ERROR] MMAP failed!!\n");
        return -EAGAIN;
    }


    return 0;
}

static const struct file_operations g_mjc_fops =
{
    .owner = THIS_MODULE,
    .open = mjc_open,
    .release = mjc_release,
    .unlocked_ioctl = mjc_ioctl,
    .mmap = mjc_mmap,
#if IS_ENABLED(CONFIG_COMPAT)
    .compat_ioctl   = compat_mjc_ioctl
#endif
};

static irqreturn_t mjc_intr_dlr(int irq, void *dev_id)
{
    unsigned long ulFlags;

	MJCDBG("mjc_intr_dlr()");

	spin_lock_irqsave(&ContextLock, ulFlags);
	_mjc_SetEvent(&(grContext.rEvent));
	spin_unlock_irqrestore(&ContextLock, ulFlags);

	spin_lock_irqsave(&HWLock, ulFlags);
	_mjc_SetEvent(&(grHWLockContext.rEvent));
	spin_unlock_irqrestore(&HWLock, ulFlags);

    
    disable_irq_nosync(gi4IrqID);    

    return IRQ_HANDLED;
}


static int mjc_probe(struct platform_device *pDev)
{
    struct device_node *node = pDev->dev.of_node;
    struct resource res;
	int ret;
	unsigned long ulFlags;

    MJCDBG("mjc_probe()");

    ret = register_chrdev_region(mjc_devno, 1, MJC_DEVNAME);
    if(ret)
    {
	    MJCMSG("[ERROR] Can't Get Major number for MJC Device\n");
    }

    g_mjc_cdev = cdev_alloc();
    g_mjc_cdev->owner = THIS_MODULE;
    g_mjc_cdev->ops = &g_mjc_fops;

    ret = cdev_add(g_mjc_cdev, mjc_devno, 1);

    
    pMjcClass = class_create(THIS_MODULE, MJC_DEVNAME);
    if (IS_ERR(pMjcClass)) {
	    ret = PTR_ERR(pMjcClass);
	    MJCMSG("Unable to create class, err = %d", ret);
	    return ret;
    }
    mjcDevice = device_create(pMjcClass, NULL, mjc_devno, NULL, MJC_DEVNAME);	

    gulRegister = (unsigned long)of_iomap(node, 0);
	gi4IrqID = irq_of_parse_and_map(node, 0);
	
    of_address_to_resource(node, 0, &res);
    gu1PaReg = res.start;
    gu1PaSize= resource_size(&res);	

	spin_lock_irqsave(&ContextLock, ulFlags);
	grContext.rEvent.u4TimeoutMs = 0xFFFFFFFF;
	_mjc_CreateEvent(&(grContext.rEvent));
	spin_unlock_irqrestore(&ContextLock, ulFlags);

	spin_lock_irqsave(&HWLock, ulFlags);
	grHWLockContext.rEvent.u4TimeoutMs = 0xFFFFFFFF;
	_mjc_CreateEvent(&(grHWLockContext.rEvent));
	spin_unlock_irqrestore(&HWLock, ulFlags);

	
	
	
	if (request_irq(gi4IrqID , (irq_handler_t)mjc_intr_dlr, IRQF_TRIGGER_LOW, MJC_DEVNAME, NULL) < 0)
	{
	    MJCMSG("[ERROR] mjc_probe() error to request dec irq\n");
	}
	else
	{
	    MJCDBG("mjc_probe() success to request dec irq\n");
	}
	disable_irq(gi4IrqID);

	return 0;
}


static int mjc_remove(struct platform_device *pdev)
{
    unsigned long ulFlags;

    MJCDBG("mjc_remove() \n");

    cdev_del(g_mjc_cdev);
    unregister_chrdev_region(mjc_devno, 1);

    
    
    free_irq(gi4IrqID , NULL);    

    

	spin_lock_irqsave(&ContextLock, ulFlags);
    _mjc_CloseEvent(&(grContext.rEvent));
	spin_unlock_irqrestore(&ContextLock, ulFlags);

	spin_lock_irqsave(&HWLock, ulFlags);
    _mjc_CloseEvent(&(grHWLockContext.rEvent));
	spin_unlock_irqrestore(&HWLock, ulFlags);

    return 0;
}

static int mjc_suspend(struct platform_device *pdev, pm_message_t mesg)
{
    MJCMSG("mjc_early_suspend, tid = %d\n", current->pid);

    MJCDBG("mjc_early_suspend - tid = %d\n", current->pid);
	return 0;
}

static int mjc_resume(struct platform_device *pdev)
{

    MJCMSG("mjc_late_resume, tid = %d\n", current->pid);

    MJCDBG("mjc_late_resume - tid = %d\n", current->pid);
	return 0;
}

#ifdef CONFIG_PM
int mjc_pm_suspend(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);

	MJCDBG("calling %s()\n", __func__);

    BUG_ON(pdev == NULL);

    return mjc_suspend(pdev, PMSG_SUSPEND);
}

int mjc_pm_resume(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);

	MJCDBG("calling %s()\n", __func__);

    BUG_ON(pdev == NULL);

    return mjc_resume(pdev);
}

extern void mt_irq_set_sens(unsigned int irq, unsigned int sens);
extern void mt_irq_set_polarity(unsigned int irq, unsigned int polarity);
static int mjc_pm_restore_noirq(struct device *device)
{
    
    
    mt_irq_set_sens(gi4IrqID, MT_LEVEL_SENSITIVE);
    mt_irq_set_polarity(gi4IrqID, MT_POLARITY_LOW);

    return 0;
}

struct dev_pm_ops mjc_pm_ops = {
    .suspend = mjc_pm_suspend,
    .resume = mjc_pm_resume,
    .freeze = mjc_pm_suspend,
    .thaw = mjc_pm_resume,
    .poweroff = mjc_pm_suspend,
    .restore = mjc_pm_resume,
    .restore_noirq = mjc_pm_restore_noirq
};
#endif 

static const struct of_device_id mjc_of_ids[] = {
	{ .compatible = "mediatek,MJC_TOP", },
	{}
};

static struct platform_driver mjcDrv = {
    .probe	= mjc_probe,
    .remove	= mjc_remove,
    .suspend= mjc_suspend,
    .resume	= mjc_resume,
    .driver	= {
    .name	= MJC_DEVNAME,
    .of_match_table = mjc_of_ids,
#ifdef CONFIG_PM
    .pm     = &mjc_pm_ops,
#endif
    .owner	= THIS_MODULE,
    }
};


static int __init mjc_driver_init(void)
{
    int cnt;
    MJCDBG("mjc_driver_init()");

	if(platform_driver_register(&mjcDrv))
	{
		MJCMSG("failed to register MAU driver");
		return -ENODEV;
    }
    
    for(cnt=0;cnt<MJC_FORCE_REG_NUM;cnt++)
    {
        gfWriteReg[cnt].reg = 0;
        gfWriteReg[cnt].val = 0;
        gfWriteReg[cnt].mask = 0;                 
    } 
	return 0;
}

static void __exit mjc_driver_exit(void)
{
    MJCDBG("mjc_driver_exit()");

    platform_driver_unregister(&mjcDrv);
}

module_init(mjc_driver_init);
module_exit(mjc_driver_exit);

MODULE_AUTHOR("Gary Huang <gary.huang@mediatek.com>");
MODULE_DESCRIPTION("MTK MJC Driver");
MODULE_LICENSE("GPL");
