/*
 *  IR remote control driver for sunxi platform (Allwinner A1X)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <plat/sys_config.h>
#include <media/rc-core.h>

#define DRIVER_NAME                "sunxi-cir"
#define DRIVER_VERS                "1.1"

/* RC5 protocol has min pulse width, so sample=8us */
/* JVC has minimum packet period so idle=30ms */
#define IR_CLOCK_RATE                8000000      /* ir clock rate (Hz) */
#define IR_SAMPLE_CLK_SEL        (0x0 << 0)   /* ir clk div (DIV = 64 << SEL) */
#define IR_RXFILT_VAL                (1)          /* Pulse threshold = 8us */
#define IR_RXIDLE_VAL                (29)         /* Idle threshold = 30ms */
#define IR_DRIVER_TIMEOUT        (30)         /* Same as above (30ms)  */
#define IR_FIFO_SIZE                (16)         /* FIFO len is 16 bytes  */
#define IR_INVERT_INPUT                (1)          /* 1 - invert, 0 - not invert  */
#define VALUE_MASK                0x80         /* Bit15 - value (pulse/space) */
#define PERIOD_MASK                0x7f         /* Bits0:14 - sample duration  */

/* IRQ num & registers base */
#define IR_IRQNO                SW_INT_IRQNO_IR0
#define IR_BASE                        SW_PA_IR0_IO_BASE
#define IR_RANGE_SIZE                200

/* Calculate sample freq and period */
#define CIR_SAMPLE_HZ                (IR_CLOCK_RATE / (64 << IR_SAMPLE_CLK_SEL))
#define CIR_SAMPLE_PERIOD        (1000000000ul / CIR_SAMPLE_HZ) /* ns */

/* IR controller registers */
#define IR_REG(x)                (x)
#define IR_CTRL_REG                IR_REG(0x00) /* IR Control */
#define IR_RXCFG_REG                IR_REG(0x10) /* Rx Config */
#define IR_RXDAT_REG                IR_REG(0x20) /* Rx Data */
#define IR_RXINTE_REG                IR_REG(0x2C) /* Rx Interrupt Enable */
#define IR_RXINTS_REG                IR_REG(0x30) /* Rx Interrupt Status */
#define IR_SPLCFG_REG                IR_REG(0x34) /* IR Sample Config */

/* Bits of IR_RXINTS_REG register */
#define IR_RXINTS_RXOF                (0x1 << 0)   /* Rx FIFO Overflow */
#define IR_RXINTS_RXPE                (0x1 << 1)   /* Rx Packet End */
#define IR_RXINTS_RXDA                (0x1 << 4)   /* Rx FIFO Data ready */

/* Bits of IR_CTRL_REG register */
#define IRDA_MODE_CIR                (0x3 << 4)   /* IRDA mode CIR (for rc) */
#define IR_RX_EN                (0x1 << 1)   /* IR receiver enable flag */
#define IR_GLOB_EN                (0x1 << 0)   /* Global IR enable flag */

/* Bits of IR_RXCFG_REG register */
#define IR_INVERT_EN                (IR_INVERT_INPUT << 2)  /* Invert input bit */

/* Bits of IR_RXINTE_REG register */
#define RPEI_EN                        (0x1 << 0)   /* recv packet end int enable */
#define RISI_EN                        (0x1 << 1)   /* recv illegal sym int enable */
#define RAI_EN                        (0x1 << 4)   /* RX FIFO available int enable */

#define SUCCESS                        (0)          /* for functions "return" op */

struct sunxi_ir_chip {
        struct rc_dev *rcdev;
        struct device *dev;
        void __iomem *gaddr;
        unsigned gpio_hdle;
        spinlock_t irq_lock;
        struct clk *apb_ir_clk;
        struct clk *ir_clk;
};

/* Configure and enable ir receiver in cir mode */
static int ir_setup(struct sunxi_ir_chip *ir_chip)
{
        unsigned long tmp = 0;

        ir_chip->gpio_hdle = gpio_request_ex("ir_para", "ir0_rx");
        if (!ir_chip->gpio_hdle) {
                pr_err("Try to request ir_para gpio failed!\n");
                return -EINVAL;
        }

        ir_chip->apb_ir_clk = clk_get(NULL, "apb_ir0");
        if (!ir_chip->apb_ir_clk) {
                pr_err("Try to get apb_ir0 clock failed!\n");
                return -EINVAL;
        }

        ir_chip->ir_clk = clk_get(NULL, "ir0");
        if (!ir_chip->ir_clk) {
                pr_err("Try to get ir0 clock failed!\n");
                return -EINVAL;
        }

        if (clk_set_rate(ir_chip->ir_clk, IR_CLOCK_RATE)) {
                pr_err("Try to set ir0 clock rate failed!\n");
                return -EINTR;
        }

        /* Should we recalculate CIR_SAMPLE_PERIOD using this ? */
        pr_info("IR clock rate: %luHz\n",
                (unsigned long)clk_get_rate(ir_chip->ir_clk));
        pr_info("IR sample period: %luns\n",
                (unsigned long)CIR_SAMPLE_PERIOD);

        if (clk_enable(ir_chip->apb_ir_clk)) {
                pr_err("Try to enable apb_ir_clk failed!\n");
                return -EINTR;
        }

        if (clk_enable(ir_chip->ir_clk)) {
                pr_err("Try to enable apb_ir_clk failed!\n");
                return -EINTR;
        }

        /* Enable CIR Mode */
        writel(IRDA_MODE_CIR, ir_chip->gaddr + IR_CTRL_REG);

        /* Config IR Smaple Register */
        tmp = IR_SAMPLE_CLK_SEL;            /* Fsample divider */
        tmp |= (IR_RXFILT_VAL & 0x3f) << 2; /* Set Filter Threshold */
        tmp |= (IR_RXIDLE_VAL & 0xff) << 8; /* Set Idle Threshold */
        writel(tmp, ir_chip->gaddr + IR_SPLCFG_REG);

        /* Set up signal inversion */
        writel(IR_INVERT_EN, ir_chip->gaddr + IR_RXCFG_REG);

        /* Clear All Rx Interrupt Status */
        writel(0xff, ir_chip->gaddr + IR_RXINTS_REG);

        /* Enable rx interrupt & set RAL = FIFOsz/2; */
        tmp = RPEI_EN | RISI_EN | RAI_EN;
        tmp |= ((IR_FIFO_SIZE >> 1) - 1) << 8;
        writel(tmp, ir_chip->gaddr + IR_RXINTE_REG);

        /* Enable IR Module */
        tmp = readl(ir_chip->gaddr + IR_CTRL_REG);
        writel((tmp | IR_GLOB_EN | IR_RX_EN), ir_chip->gaddr + IR_CTRL_REG);

        return SUCCESS;
}

/* Disable ir receiver, stop clock, free gpio */
static void ir_stop(struct sunxi_ir_chip *ir_chip)
{
        /* Disable Rx Interrupts */
        writel(0, ir_chip->gaddr + IR_RXINTE_REG);

        /* Clear All Rx Interrupt Status */
        writel(0xff, ir_chip->gaddr + IR_RXINTS_REG);

        /* Disable IR Module */
        writel(0, ir_chip->gaddr + IR_CTRL_REG);

        if (ir_chip->ir_clk)
                clk_disable(ir_chip->ir_clk);
        if (ir_chip->apb_ir_clk)
                clk_disable(ir_chip->apb_ir_clk);

        gpio_release(ir_chip->gpio_hdle, 1);
}

/* STUB: Protocol change callback (this can be used */
/* to ajust ir controller settings for protocols)   */
int change_protocol(struct rc_dev *dev, u64 rc_type)
{
        /* pr_info("Proto_change to [0x%08lX]\n", (unsigned long)rc_type); */
        return SUCCESS;
}

/* IR controller interrupt handler */
static irqreturn_t sunxi_ir_irq(int irq, void *devid)
{
        int i = 0;
        u8 gval = 0;
        u8 scnt = 0;
        __u32 intsta = 0;
        unsigned long flags;
        DEFINE_IR_RAW_EVENT(rawir);
        struct sunxi_ir_chip *ir_chip = devid;

        /* Read & clear interrupt status */
        spin_lock_irqsave(&ir_chip->irq_lock, flags);
        intsta = readl(ir_chip->gaddr + IR_RXINTS_REG);
        writel(intsta & 0xff, ir_chip->gaddr + IR_RXINTS_REG);

        /* Status bits 7:15 - number of fifo samples available */
        scnt = (intsta >> 8) & 0xff;

        /* Read all data from FIFO buffer */
        init_ir_raw_event(&rawir);
        for (i = 0; i < scnt; i++) {
                gval = (u8)(readl(ir_chip->gaddr + IR_RXDAT_REG));
                rawir.pulse = ((gval & VALUE_MASK) != 0);
                rawir.duration = (gval & PERIOD_MASK) * CIR_SAMPLE_PERIOD;
                ir_raw_event_store_with_filter(ir_chip->rcdev, &rawir);
                ir_raw_event_handle(ir_chip->rcdev);
        }

        /* Set idle on END_OF_PACKET event */
        if (intsta & IR_RXINTS_RXPE)
                ir_raw_event_set_idle(ir_chip->rcdev, true);

        /* FIFO Overflow hardware event */
        if (intsta & IR_RXINTS_RXOF)
                ir_raw_event_reset(ir_chip->rcdev);

        spin_unlock_irqrestore(&ir_chip->irq_lock, flags);
        return IRQ_HANDLED;
}

/* Initialization: Create rc device, request irq, setup ir  */
static int __devinit sunxi_ir_probe(struct platform_device *pdev)
{
        struct sunxi_ir_chip *ir_chip;
        int err = 0;

        ir_chip = kzalloc(sizeof(struct sunxi_ir_chip), GFP_KERNEL);
        if (!ir_chip) {
                pr_err("%s kzalloc failed\n", __func__);
                err = -ENOMEM;
                goto exit;
        }

        ir_chip->rcdev = rc_allocate_device();
        if (!ir_chip->rcdev) {
                err = -ENOMEM;
                goto err_allocate;
        }

        ir_chip->rcdev->driver_name = DRIVER_NAME;
        ir_chip->rcdev->input_name = DRIVER_NAME;
        ir_chip->rcdev->driver_type = RC_DRIVER_IR_RAW;
        ir_chip->rcdev->input_id.bustype = BUS_HOST;
        ir_chip->rcdev->map_name = RC_MAP_EMPTY;
        ir_chip->rcdev->allowed_protos = RC_TYPE_ALL;
        ir_chip->rcdev->rx_resolution = CIR_SAMPLE_PERIOD;
        ir_chip->rcdev->timeout = MS_TO_NS(IR_DRIVER_TIMEOUT);
        ir_chip->rcdev->change_protocol = change_protocol;

        err = rc_register_device(ir_chip->rcdev);
        if (err < 0) {
                pr_err("Failed to register rc device!\n");
                goto err_register;
        }

        err = request_irq(IR_IRQNO, sunxi_ir_irq, 0, DRIVER_NAME, ir_chip);
        if (err < 0) {
                pr_err("Can't request irq %d!\n", IR_IRQNO);
                goto err_irq;
        }

        ir_chip->gaddr = ioremap(IR_BASE, IR_RANGE_SIZE);
        if (!ir_chip->gaddr) {
                pr_err("Can't request ir registers memory!\n");
                err = -EIO;
                goto err_map;
        }

        platform_set_drvdata(pdev, ir_chip);
        ir_raw_event_reset(ir_chip->rcdev);
        err = ir_setup(ir_chip);
        if (err != 0)
                goto err_setup;

        return SUCCESS;

err_setup:
        ir_stop(ir_chip);
        iounmap(ir_chip->gaddr);
err_map:
        free_irq(IR_IRQNO, ir_chip);
err_irq:
        rc_unregister_device(ir_chip->rcdev);
err_register:
        rc_free_device(ir_chip->rcdev);
err_allocate:
        kfree(ir_chip);
exit:
        return err;
}

/* Remove ir device & driver */
static int __devexit sunxi_ir_remove(struct platform_device *pdev)
{
        struct sunxi_ir_chip *ir_chip = platform_get_drvdata(pdev);

        ir_stop(ir_chip);
        iounmap(ir_chip->gaddr);
        free_irq(IR_IRQNO, ir_chip);
        rc_unregister_device(ir_chip->rcdev);
        rc_free_device(ir_chip->rcdev);
        kfree(ir_chip);

        return SUCCESS;
}

static void sunxi_ir_release(struct device *dev)
{
        /* Nothing */
}

struct platform_device sunxi_ir_device = {
        .name                = DRIVER_NAME,
        .id                = -1,
        .dev =  {
                .release = sunxi_ir_release,
                },
};

static struct platform_driver sunxi_ir_driver = {
        .driver.name        = DRIVER_NAME,
        .driver.owner        = THIS_MODULE,
        .probe                = sunxi_ir_probe,
        .remove                = __devexit_p(sunxi_ir_remove),
};

static int __init sunxi_ir_init(void)
{
        int err = 0;

        pr_info("Device driver %s version %s\n", DRIVER_NAME, DRIVER_VERS);
        err = platform_device_register(&sunxi_ir_device);
        if (err)
                goto exit;

        err = platform_driver_register(&sunxi_ir_driver);
        if (err)
                platform_device_unregister(&sunxi_ir_device);

exit:
        return err;
}
module_init(sunxi_ir_init);

static void __exit sunxi_ir_exit(void)
{
        platform_driver_unregister(&sunxi_ir_driver);
        platform_device_unregister(&sunxi_ir_device);
}
module_exit(sunxi_ir_exit);

MODULE_AUTHOR("Alexandr Shutko <al...@shutko.ru>");
MODULE_DESCRIPTION("CIR interface for Allwinner A1X SOCs");
MODULE_LICENSE("GPL");
