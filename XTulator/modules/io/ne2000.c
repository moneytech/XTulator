#include "../../config.h"

#ifdef USE_NE2000

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "../../chipset/i8259.h"
#include "../../ports.h"
#include "../../debuglog.h"
#include "ne2000.h"

//#define pclog printf
#ifdef DEBUG_NE2000
#define pclog(...) debug_log(DEBUG_DETAIL, __VA_ARGS__)
#else
#define pclog(...) (nulldump, __VA_ARGS__)
uint8_t nulldump[2048];
#endif

#define fatal printf

#define BX_RESET_HARDWARE 0
#define BX_RESET_SOFTWARE 1

//Never completely fill the ne2k ring so that we never
// hit the unclear completely full buffer condition.
#define BX_NE2K_NEVER_FULL_RING (1)

uint8_t rtl8029as_eeprom[128];

NE2000_t ne2000_dev;

static void NE2000_tx_event(int val, void* p);

void ne2000_rx_frame(void* p, const void* buf, int io_len);

int ne2000_can_receive() {
    return 1;
}

void ne2000_receive(const uint8_t* buf, int size) {
    ne2000_rx_frame(&ne2000_dev, buf, size);
}

static void ne2000_setirq(NE2000_t* ne2000, int irq)
{
    ne2000->base_irq = irq;
}
//
// reset - restore state to power-up, cancelling all i/o
//
static void ne2000_reset(int type, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;
    int i;

    pclog("ne2000 reset\n");


    // Initialise the mac address area by doubling the physical address
    ne2000->macaddr[0] = ne2000->physaddr[0];
    ne2000->macaddr[1] = ne2000->physaddr[0];
    ne2000->macaddr[2] = ne2000->physaddr[1];
    ne2000->macaddr[3] = ne2000->physaddr[1];
    ne2000->macaddr[4] = ne2000->physaddr[2];
    ne2000->macaddr[5] = ne2000->physaddr[2];
    ne2000->macaddr[6] = ne2000->physaddr[3];
    ne2000->macaddr[7] = ne2000->physaddr[3];
    ne2000->macaddr[8] = ne2000->physaddr[4];
    ne2000->macaddr[9] = ne2000->physaddr[4];
    ne2000->macaddr[10] = ne2000->physaddr[5];
    ne2000->macaddr[11] = ne2000->physaddr[5];

    // ne2k signature
    for (i = 12; i < 32; i++)
        ne2000->macaddr[i] = 0x57;

    // Zero out registers and memory
    memset(&ne2000->CR, 0, sizeof(ne2000->CR));
    memset(&ne2000->ISR, 0, sizeof(ne2000->ISR));
    memset(&ne2000->IMR, 0, sizeof(ne2000->IMR));
    memset(&ne2000->DCR, 0, sizeof(ne2000->DCR));
    memset(&ne2000->TCR, 0, sizeof(ne2000->TCR));
    memset(&ne2000->TSR, 0, sizeof(ne2000->TSR));
    //memset( & ne2000->RCR, 0, sizeof(ne2000->RCR));
    memset(&ne2000->RSR, 0, sizeof(ne2000->RSR));
    ne2000->tx_timer_active = 0;
    ne2000->local_dma = 0;
    ne2000->page_start = 0;
    ne2000->page_stop = 0;
    ne2000->bound_ptr = 0;
    ne2000->tx_page_start = 0;
    ne2000->num_coll = 0;
    ne2000->tx_bytes = 0;
    ne2000->fifo = 0;
    ne2000->remote_dma = 0;
    ne2000->remote_start = 0;
    ne2000->remote_bytes = 0;
    ne2000->tallycnt_0 = 0;
    ne2000->tallycnt_1 = 0;
    ne2000->tallycnt_2 = 0;

    //memset( & ne2000->physaddr, 0, sizeof(ne2000->physaddr));
    //memset( & ne2000->mchash, 0, sizeof(ne2000->mchash));
    ne2000->curr_page = 0;

    ne2000->rempkt_ptr = 0;
    ne2000->localpkt_ptr = 0;
    ne2000->address_cnt = 0;

    memset(&ne2000->mem, 0, sizeof(ne2000->mem));

    // Set power-up conditions
    ne2000->CR.stop = 1;
    ne2000->CR.rdma_cmd = 4;
    ne2000->ISR.reset = 1;
    ne2000->DCR.longaddr = 1;

    i8259_doirq(ne2000->i8259, ne2000->base_irq);
    //picint(1 << ne2000->base_irq);
    //picintc(1 << ne2000->base_irq);
    //DEV_pic_lower_irq(ne2000->base_irq);
}

#include "bswap.h"

//
// chipmem_read/chipmem_write - access the 64K private RAM.
// The ne2000 memory is accessed through the data port of
// the asic (offset 0) after setting up a remote-DMA transfer.
// Both byte and word accesses are allowed.
// The first 16 bytes contains the MAC address at even locations,
// and there is 16K of buffer memory starting at 16K
//
uint8_t ne2000_chipmem_read_b(uint32_t address, NE2000_t* ne2000)
{
    // ROM'd MAC address
    if ((address >= 0) && (address <= 31)) {
        return ne2000->macaddr[address];
    }

    if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND)) {
        return ne2000->mem[address - BX_NE2K_MEMSTART];
    }
    else {
        return (0xff);
    }
}


uint16_t ne2000_chipmem_read_w(uint32_t address, NE2000_t* ne2000)
{
    // ROM'd MAC address
    if ((address >= 0) && (address <= 31)) {
        return le16_to_cpu(*(uint16_t*)(ne2000->macaddr + address));
    }

    if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND)) {
        return le16_to_cpu(*(uint16_t*)(ne2000->mem + (address - BX_NE2K_MEMSTART)));
    }
    else {
        return (0xffff);
    }
}

void ne2000_chipmem_write_b(uint32_t address, uint8_t value, NE2000_t* ne2000)
{
    if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND)) {
        ne2000->mem[address - BX_NE2K_MEMSTART] = value & 0xff;
    }
}


void ne2000_chipmem_write_w(uint32_t address, uint16_t value, NE2000_t* ne2000)
{
    if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND)) {
        *(uint16_t*)(ne2000->mem + (address - BX_NE2K_MEMSTART)) = cpu_to_le16(value);
    }
}

//
// asic_read/asic_write - This is the high 16 bytes of i/o space
// (the lower 16 bytes is for the DS8390). Only two locations
// are used: offset 0, which is used for data transfer, and
// offset 0xf, which is used to reset the device.
// The data transfer port is used to as 'external' DMA to the
// DS8390. The chip has to have the DMA registers set up, and
// after that, insw/outsw instructions can be used to move
// the appropriate number of bytes to/from the device.
//
uint16_t ne2000_dma_read(int io_len, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;
    //
    // The 8390 bumps the address and decreases the byte count
    // by the selected word size after every access, not by
    // the amount of data requested by the host (io_len).
    //
    ne2000->remote_dma += io_len;
    if (ne2000->remote_dma == ne2000->page_stop << 8) {
        ne2000->remote_dma = ne2000->page_start << 8;
    }
    // keep s.remote_bytes from underflowing
    if (ne2000->remote_bytes > 1)
        ne2000->remote_bytes -= io_len;
    else
        ne2000->remote_bytes = 0;

    // If all bytes have been written, signal remote-DMA complete
    if (ne2000->remote_bytes == 0) {
        ne2000->ISR.rdma_done = 1;
        if (ne2000->IMR.rdma_inte) {
            i8259_doirq(ne2000->i8259, ne2000->base_irq);
            //doirq(ne2000->base_irq);
            //picint(1 << ne2000->base_irq);
            //picintc(1 << ne2000->base_irq);
    //DEV_pic_raise_irq(ne2000->base_irq);
        }
    }
    return (0);
}

uint16_t ne2000_asic_read_w(uint32_t offset, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;
    int retval;

    if ((ne2000->DCR.wdsize & 0x01) && (ne2000->remote_dma < 32)) { //todo: why do i need this hack??
        /* 16 bit access */
        retval = ne2000_chipmem_read_w(ne2000->remote_dma, ne2000);
        ne2000_dma_read(2, ne2000);
    }
    else {
        /* 8 bit access */
        retval = ne2000_chipmem_read_b(ne2000->remote_dma, ne2000);
        ne2000_dma_read(1, ne2000);
    }

    pclog("asic read val=0x%04x\n", retval);

    return retval;
}

void ne2000_dma_write(int io_len, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;

    // is this right ??? asic_read uses DCR.wordsize
    ne2000->remote_dma += io_len;
    if (ne2000->remote_dma == ne2000->page_stop << 8) {
        ne2000->remote_dma = ne2000->page_start << 8;
    }

    ne2000->remote_bytes -= io_len;
    if (ne2000->remote_bytes > BX_NE2K_MEMSIZ)
        ne2000->remote_bytes = 0;

    // If all bytes have been written, signal remote-DMA complete
    if (ne2000->remote_bytes == 0) {
        ne2000->ISR.rdma_done = 1;
        if (ne2000->IMR.rdma_inte) {
            i8259_doirq(ne2000->i8259, ne2000->base_irq);
            //doirq(ne2000->base_irq);
            //picint(1 << ne2000->base_irq);
            //picintc(1 << ne2000->base_irq);
            //DEV_pic_raise_irq(ne2000->base_irq);
        }
    }
}

void ne2000_asic_write_w(uint32_t offset, uint16_t value, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;

    pclog("asic write val=0x%04x\n", value);

    if (ne2000->remote_bytes == 0)
        return;
    if ((ne2000->DCR.wdsize & 0x01) && (ne2000->remote_dma < 32)) { //todo: why do i need this hack??
        /* 16 bit access */
        ne2000_chipmem_write_w(ne2000->remote_dma, value, ne2000);
        ne2000_dma_write(2, ne2000);
    }
    else {
        /* 8 bit access */
        ne2000_chipmem_write_b(ne2000->remote_dma, value, ne2000);
        ne2000_dma_write(1, ne2000);
    }
}

uint8_t ne2000_asic_read_b(uint32_t offset, void* p)
{
    if (offset & 1)
        return ne2000_asic_read_w(offset & ~1, p) >> 1;
    return ne2000_asic_read_w(offset, p) & 0xff;
}

void ne2000_asic_write_b(uint32_t offset, uint8_t value, void* p)
{
    if (offset & 1)
        ne2000_asic_write_w(offset & ~1, value << 8, p);
    else
        ne2000_asic_write_w(offset, value, p);
}

uint16_t ne2000_reset_read(uint32_t offset, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;
    ne2000_reset(BX_RESET_SOFTWARE, ne2000);
    return 0;
}

void ne2000_reset_write(uint32_t offset, uint16_t value, void* p)
{
}

//
// read_handler/read - i/o 'catcher' function called from BOCHS
// mainline when the CPU attempts a read in the i/o space registered
// by this ne2000 instance
//
uint16_t ne2000_read(uint32_t address, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;
    int ret;

    pclog("read addr %x\n", address);

    address &= 0xf;

    if (address == 0x00) {
        ret =
            (((ne2000->CR.pgsel & 0x03) << 6) |
                ((ne2000->CR.rdma_cmd & 0x07) << 3) |
                (ne2000->CR.tx_packet << 2) |
                (ne2000->CR.start << 1) |
                (ne2000->CR.stop));
        pclog("read CR returns 0x%08x\n", ret);
    }
    else {
        switch (ne2000->CR.pgsel) {
        case 0x00:
            pclog("page 0 read from port %04x\n", address);

            switch (address) {
            case 0x1:  // CLDA0
                return (ne2000->local_dma & 0xff);
                break;

            case 0x2:  // CLDA1
                return (ne2000->local_dma >> 8);
                break;

            case 0x3:  // BNRY
                return (ne2000->bound_ptr);
                break;

            case 0x4:  // TSR
                return ((ne2000->TSR.ow_coll << 7) |
                    (ne2000->TSR.cd_hbeat << 6) |
                    (ne2000->TSR.fifo_ur << 5) |
                    (ne2000->TSR.no_carrier << 4) |
                    (ne2000->TSR.aborted << 3) |
                    (ne2000->TSR.collided << 2) |
                    (ne2000->TSR.tx_ok));
                break;

            case 0x5:  // NCR
                return (ne2000->num_coll);
                break;

            case 0x6:  // FIFO
              // reading FIFO is only valid in loopback mode
                pclog("reading FIFO not supported yet\n");
                return (ne2000->fifo);
                break;

            case 0x7:  // ISR
                return ((ne2000->ISR.reset << 7) |
                    (ne2000->ISR.rdma_done << 6) |
                    (ne2000->ISR.cnt_oflow << 5) |
                    (ne2000->ISR.overwrite << 4) |
                    (ne2000->ISR.tx_err << 3) |
                    (ne2000->ISR.rx_err << 2) |
                    (ne2000->ISR.pkt_tx << 1) |
                    (ne2000->ISR.pkt_rx));
                break;

            case 0x8:  // CRDA0
                return (ne2000->remote_dma & 0xff);
                break;

            case 0x9:  // CRDA1
                return (ne2000->remote_dma >> 8);
                break;

            case 0xa:  // reserved
                pclog("reserved read - page 0, 0xa\n");
                //if (network_card_current == 2)  return ne2000->i8029id0; //todo
                return (0xff);
                break;

            case 0xb:  // reserved
                pclog("reserved read - page 0, 0xb\n");
                //if (network_card_current == 2)  return ne2000->i8029id1; //todo
                return (0xff);
                break;

            case 0xc:  // RSR
                return ((ne2000->RSR.deferred << 7) |
                    (ne2000->RSR.rx_disabled << 6) |
                    (ne2000->RSR.rx_mbit << 5) |
                    (ne2000->RSR.rx_missed << 4) |
                    (ne2000->RSR.fifo_or << 3) |
                    (ne2000->RSR.bad_falign << 2) |
                    (ne2000->RSR.bad_crc << 1) |
                    (ne2000->RSR.rx_ok));
                break;

            case 0xd:  // CNTR0
                return (ne2000->tallycnt_0);
                break;

            case 0xe:  // CNTR1
                return (ne2000->tallycnt_1);
                break;

            case 0xf:  // CNTR2
                return (ne2000->tallycnt_2);
                break;
            }

            return(0);
            break;

        case 0x01:
            pclog("page 1 read from port %04x\n", address);

            switch (address) {
            case 0x1:  // PAR0-5
            case 0x2:
            case 0x3:
            case 0x4:
            case 0x5:
            case 0x6:
                return (ne2000->physaddr[address - 1]);
                break;

            case 0x7:  // CURR
                pclog("returning current page: %02x\n", (ne2000->curr_page));
                return (ne2000->curr_page);

            case 0x8:  // MAR0-7
            case 0x9:
            case 0xa:
            case 0xb:
            case 0xc:
            case 0xd:
            case 0xe:
            case 0xf:
                return (ne2000->mchash[address - 8]);
                break;
            }

            return (0);
            break;

        case 0x02:
            pclog("page 2 read from port %04x\n", address);

            switch (address) {
            case 0x1:  // PSTART
                return (ne2000->page_start);
                break;

            case 0x2:  // PSTOP
                return (ne2000->page_stop);
                break;

            case 0x3:  // Remote Next-packet pointer
                return (ne2000->rempkt_ptr);
                break;

            case 0x4:  // TPSR
                return (ne2000->tx_page_start);
                break;

            case 0x5:  // Local Next-packet pointer
                return (ne2000->localpkt_ptr);
                break;

            case 0x6:  // Address counter (upper)
                return (ne2000->address_cnt >> 8);
                break;

            case 0x7:  // Address counter (lower)
                return (ne2000->address_cnt & 0xff);
                break;

            case 0x8:  // Reserved
            case 0x9:
            case 0xa:
            case 0xb:
                pclog("reserved read - page 2, 0x%02x\n", address);
                break;

            case 0xc:  // RCR
                return ((ne2000->RCR.monitor << 5) |
                    (ne2000->RCR.promisc << 4) |
                    (ne2000->RCR.multicast << 3) |
                    (ne2000->RCR.broadcast << 2) |
                    (ne2000->RCR.runts_ok << 1) |
                    (ne2000->RCR.errors_ok));
                break;

            case 0xd:  // TCR
                return ((ne2000->TCR.coll_prio << 4) |
                    (ne2000->TCR.ext_stoptx << 3) |
                    ((ne2000->TCR.loop_cntl & 0x3) << 1) |
                    (ne2000->TCR.crc_disable));
                break;

            case 0xe:  // DCR
                return (((ne2000->DCR.fifo_size & 0x3) << 5) |
                    (ne2000->DCR.auto_rx << 4) |
                    (ne2000->DCR.loop << 3) |
                    (ne2000->DCR.longaddr << 2) |
                    (ne2000->DCR.endian << 1) |
                    (ne2000->DCR.wdsize));
                break;

            case 0xf:  // IMR
                return ((ne2000->IMR.rdma_inte << 6) |
                    (ne2000->IMR.cofl_inte << 5) |
                    (ne2000->IMR.overw_inte << 4) |
                    (ne2000->IMR.txerr_inte << 3) |
                    (ne2000->IMR.rxerr_inte << 2) |
                    (ne2000->IMR.tx_inte << 1) |
                    (ne2000->IMR.rx_inte));
                break;
            }
            break;

        case 3:
            //if (network_card_current == 1)  fatal("ne2000 unknown value of pgsel in read - %d\n", ne2000->CR.pgsel); //todo

            switch (address)
            {
            case 0:
                return ne2000->cr;
            case 1:
                return ne2000->i9346cr;
            case 3:
                return ne2000->config0;
            case 5:
                return ne2000->config2;
            case 6:
                return ne2000->config3;
            case 9:
                return 0xFF;
            case 0xE:
                return ne2000->i8029asid0;
            case 0xF:
                return ne2000->i8029asid1;
            }
            break;

        default:
            fatal("ne2000 unknown value of pgsel in read - %d\n", ne2000->CR.pgsel);
        }
    }
    return ret;
}

//
// write_handler/write - i/o 'catcher' function called from BOCHS
// mainline when the CPU attempts a write in the i/o space registered
// by this ne2000 instance
//
void ne2000_write(uint32_t address, uint16_t value, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;

    pclog("write address %x, val=%x\n", address, value);

    address &= 0xf;

    //
    // The high 16 bytes of i/o space are for the ne2000 asic -
    //  the low 16 bytes are for the DS8390, with the current
    //  page being selected by the PS0,PS1 registers in the
    //  command register
    //
    if (address == 0x00) {
        pclog("wrote 0x%02x to CR\n", value);

        // Validate remote-DMA
        if ((value & 0x38) == 0x00) {
            pclog("CR write - invalid rDMA value 0\n");
            value |= 0x20; /* dma_cmd == 4 is a safe default */
                //value = 0x22; /* dma_cmd == 4 is a safe default */
        }

        // Check for s/w reset
        if (value & 0x01) {
            ne2000->ISR.reset = 1;
            ne2000->CR.stop = 1;
        }
        else {
            ne2000->CR.stop = 0;
        }

        ne2000->CR.rdma_cmd = (value & 0x38) >> 3;

        // If start command issued, the RST bit in the ISR
        // must be cleared
        if ((value & 0x02) && !ne2000->CR.start) {
            ne2000->ISR.reset = 0;
        }

        ne2000->CR.start = ((value & 0x02) == 0x02);
        ne2000->CR.pgsel = (value & 0xc0) >> 6;

        // Check for send-packet command
        if (ne2000->CR.rdma_cmd == 3) {
            // Set up DMA read from receive ring
            ne2000->remote_start = ne2000->remote_dma =
                ne2000->bound_ptr * 256;
            ne2000->remote_bytes = *((uint16_t*)&
                ne2000->mem[ne2000->bound_ptr * 256 + 2 - BX_NE2K_MEMSTART]);
            pclog("Sending buffer #x%x length %d\n",
                ne2000->remote_start,
                ne2000->remote_bytes);
        }

        // Check for start-tx
        if ((value & 0x04) && ne2000->TCR.loop_cntl) {
            // loopback mode
            if (ne2000->TCR.loop_cntl != 1) {
                pclog("Loop mode %d not supported.\n", ne2000->TCR.loop_cntl);
            }
            else {
                ne2000_rx_frame(ne2000, &ne2000->mem[ne2000->tx_page_start * 256 -
                    BX_NE2K_MEMSTART],
                    ne2000->tx_bytes);

                // do a TX interrupt
                // Generate an interrupt if not masked and not one in progress
                if (ne2000->IMR.tx_inte && !ne2000->ISR.pkt_tx) {
                    //LOG_MSG("tx complete interrupt");
                    i8259_doirq(ne2000->i8259, ne2000->base_irq);
                    //doirq(ne2000->base_irq);
                    //picint(1 << ne2000->base_irq);
                }
                ne2000->ISR.pkt_tx = 1;
            }
        }
        else if (value & 0x04) {
            // start-tx and no loopback
            if (ne2000->CR.stop || !ne2000->CR.start)
                pclog("CR write - tx start, dev in reset\n");

            if (ne2000->tx_bytes == 0)
                pclog("CR write - tx start, tx bytes == 0\n");

            // Send the packet to the system driver
                /* TODO: Transmit packet */
            //BX_NE2K_THIS ethdev->sendpkt(& ne2000->mem[ne2000->tx_page_start*256 - BX_NE2K_MEMSTART], ne2000->tx_bytes);
                //pcap_sendpacket(adhandle,&ne2000->mem[ne2000->tx_page_start*256 - BX_NE2K_MEMSTART], ne2000->tx_bytes);
            //sendpkt(&ne2000->mem[ne2000->tx_page_start * 256 - BX_NE2K_MEMSTART], ne2000->tx_bytes);
            //TODO: add actual packet sending in XTulator with pcap
            debug_log(DEBUG_INFO, "send packet\r\n");
            {
                uint32_t i, j;
                j = (ne2000->tx_page_start * 256 - BX_NE2K_MEMSTART) + ne2000->tx_bytes;
                for (i = (ne2000->tx_page_start * 256 - BX_NE2K_MEMSTART); i < j; i++) {
                    debug_log(DEBUG_INFO, "%02X ", ne2000->mem[i]);
                }
                debug_log(DEBUG_INFO, "\r\n\r\n");
            }

            NE2000_tx_event(value, ne2000);
            // Schedule a timer to trigger a tx-complete interrupt
            // The number of microseconds is the bit-time / 10.
            // The bit-time is the preamble+sfd (64 bits), the
            // inter-frame gap (96 bits), the CRC (4 bytes), and the
            // the number of bits in the frame (s.tx_bytes * 8).
            //

                /* TODO: Code transmit timer */
                /*
            bx_pc_system.activate_timer(ne2000->tx_timer_index,
                                        (64 + 96 + 4*8 + ne2000->tx_bytes*8)/10,
                                        0); // not continuous
                */
        } // end transmit-start branch

        // Linux probes for an interrupt by setting up a remote-DMA read
        // of 0 bytes with remote-DMA completion interrupts enabled.
        // Detect this here
        if (ne2000->CR.rdma_cmd == 0x01 &&
            ne2000->CR.start &&
            ne2000->remote_bytes == 0) {
            ne2000->ISR.rdma_done = 1;
            if (ne2000->IMR.rdma_inte) {
                i8259_doirq(ne2000->i8259, ne2000->base_irq);
                //doirq(ne2000->base_irq);
                //picint(1 << ne2000->base_irq);
                //picintc(1 << ne2000->base_irq);
      //DEV_pic_raise_irq(ne2000->base_irq);
            }
        }
    }
    else {
        switch (ne2000->CR.pgsel) {
        case 0x00:
            pclog("page 0 write to port %04x\n", address);

            // It appears to be a common practice to use outw on page0 regs...

            switch (address) {
            case 0x1:  // PSTART
                ne2000->page_start = value;
                break;

            case 0x2:  // PSTOP
                  // BX_INFO(("Writing to PSTOP: %02x", value));
                ne2000->page_stop = value;
                break;

            case 0x3:  // BNRY
                ne2000->bound_ptr = value;
                break;

            case 0x4:  // TPSR
                ne2000->tx_page_start = value;
                break;

            case 0x5:  // TBCR0
              // Clear out low byte and re-insert
                ne2000->tx_bytes &= 0xff00;
                ne2000->tx_bytes |= (value & 0xff);
                break;

            case 0x6:  // TBCR1
              // Clear out high byte and re-insert
                ne2000->tx_bytes &= 0x00ff;
                ne2000->tx_bytes |= ((value & 0xff) << 8);
                break;

            case 0x7:  // ISR
                value &= 0x7f;  // clear RST bit - status-only bit
                // All other values are cleared iff the ISR bit is 1
                ne2000->ISR.pkt_rx &= ~((int)((value & 0x01) == 0x01));
                ne2000->ISR.pkt_tx &= ~((int)((value & 0x02) == 0x02));
                ne2000->ISR.rx_err &= ~((int)((value & 0x04) == 0x04));
                ne2000->ISR.tx_err &= ~((int)((value & 0x08) == 0x08));
                ne2000->ISR.overwrite &= ~((int)((value & 0x10) == 0x10));
                ne2000->ISR.cnt_oflow &= ~((int)((value & 0x20) == 0x20));
                ne2000->ISR.rdma_done &= ~((int)((value & 0x40) == 0x40));
                value = ((ne2000->ISR.rdma_done << 6) |
                    (ne2000->ISR.cnt_oflow << 5) |
                    (ne2000->ISR.overwrite << 4) |
                    (ne2000->ISR.tx_err << 3) |
                    (ne2000->ISR.rx_err << 2) |
                    (ne2000->ISR.pkt_tx << 1) |
                    (ne2000->ISR.pkt_rx));
                value &= ((ne2000->IMR.rdma_inte << 6) |
                    (ne2000->IMR.cofl_inte << 5) |
                    (ne2000->IMR.overw_inte << 4) |
                    (ne2000->IMR.txerr_inte << 3) |
                    (ne2000->IMR.rxerr_inte << 2) |
                    (ne2000->IMR.tx_inte << 1) |
                    (ne2000->IMR.rx_inte));
                if (value == 0)
                    i8259_doirq(ne2000->i8259, ne2000->base_irq);
                    //doirq(ne2000->base_irq);
                //picintc(1 << ne2000->base_irq);
            //DEV_pic_lower_irq(ne2000->base_irq);
                break;

            case 0x8:  // RSAR0
              // Clear out low byte and re-insert
                ne2000->remote_start &= 0xff00;
                ne2000->remote_start |= (value & 0xff);
                ne2000->remote_dma = ne2000->remote_start;
                break;

            case 0x9:  // RSAR1
              // Clear out high byte and re-insert
                ne2000->remote_start &= 0x00ff;
                ne2000->remote_start |= ((value & 0xff) << 8);
                ne2000->remote_dma = ne2000->remote_start;
                break;

            case 0xa:  // RBCR0
              // Clear out low byte and re-insert
                ne2000->remote_bytes &= 0xff00;
                ne2000->remote_bytes |= (value & 0xff);
                break;

            case 0xb:  // RBCR1
              // Clear out high byte and re-insert
                ne2000->remote_bytes &= 0x00ff;
                ne2000->remote_bytes |= ((value & 0xff) << 8);
                break;

            case 0xc:  // RCR
              // Check if the reserved bits are set
                if (value & 0xc0)
                    pclog("RCR write, reserved bits set\n");

                // Set all other bit-fields
                ne2000->RCR.errors_ok = ((value & 0x01) == 0x01);
                ne2000->RCR.runts_ok = ((value & 0x02) == 0x02);
                ne2000->RCR.broadcast = ((value & 0x04) == 0x04);
                ne2000->RCR.multicast = ((value & 0x08) == 0x08);
                ne2000->RCR.promisc = ((value & 0x10) == 0x10);
                ne2000->RCR.monitor = ((value & 0x20) == 0x20);

                // Monitor bit is a little suspicious...
                if (value & 0x20)
                    pclog("RCR write, monitor bit set!\n");
                break;

            case 0xd:  // TCR
              // Check reserved bits
                if (value & 0xe0)
                    pclog("TCR write, reserved bits set\n");

                // Test loop mode (not supported)
                if (value & 0x06) {
                    ne2000->TCR.loop_cntl = (value & 0x6) >> 1;
                    pclog("TCR write, loop mode %d not supported\n", ne2000->TCR.loop_cntl);
                }
                else {
                    ne2000->TCR.loop_cntl = 0;
                }

                // Inhibit-CRC not supported.
                if (value & 0x01)
                {
                    //fatal("ne2000 TCR write, inhibit-CRC not supported\n");
                    pclog("ne2000 TCR write, inhibit-CRC not supported\n");
                    return;
                }

                // Auto-transmit disable very suspicious
                if (value & 0x08) {
                    //fatal("ne2000 TCR write, auto transmit disable not supported\n");
                    pclog("ne2000 TCR write, auto transmit disable not supported\n");
                }

                // Allow collision-offset to be set, although not used
                ne2000->TCR.coll_prio = ((value & 0x08) == 0x08);
                break;

            case 0xe:  // DCR
              // the loopback mode is not suppported yet
                if (!(value & 0x08)) {
                    pclog("DCR write, loopback mode selected\n");
                }
                // It is questionable to set longaddr and auto_rx, since they
                // aren't supported on the ne2000. Print a warning and continue
                if (value & 0x04)
                    pclog("DCR write - LAS set ???\n");
                if (value & 0x10)
                    pclog("DCR write - AR set ???\n");

                // Set other values.
                ne2000->DCR.wdsize = ((value & 0x01) == 0x01);
                ne2000->DCR.endian = ((value & 0x02) == 0x02);
                ne2000->DCR.longaddr = ((value & 0x04) == 0x04); // illegal ?
                ne2000->DCR.loop = ((value & 0x08) == 0x08);
                ne2000->DCR.auto_rx = ((value & 0x10) == 0x10); // also illegal ?
                ne2000->DCR.fifo_size = (value & 0x50) >> 5;
                break;

            case 0xf:  // IMR
              // Check for reserved bit
                if (value & 0x80)
                    pclog("IMR write, reserved bit set\n");

                // Set other values
                ne2000->IMR.rx_inte = ((value & 0x01) == 0x01);
                ne2000->IMR.tx_inte = ((value & 0x02) == 0x02);
                ne2000->IMR.rxerr_inte = ((value & 0x04) == 0x04);
                ne2000->IMR.txerr_inte = ((value & 0x08) == 0x08);
                ne2000->IMR.overw_inte = ((value & 0x10) == 0x10);
                ne2000->IMR.cofl_inte = ((value & 0x20) == 0x20);
                ne2000->IMR.rdma_inte = ((value & 0x40) == 0x40);
                if (ne2000->ISR.pkt_tx && ne2000->IMR.tx_inte) {
                    pclog("tx irq retrigger\n");
                    i8259_doirq(ne2000->i8259, ne2000->base_irq);
                    //doirq(ne2000->base_irq);
                    //picint(1 << ne2000->base_irq);
                    //picintc(1 << ne2000->base_irq);
                }
                break;
            }
            break;

        case 0x01:
            pclog("page 1 w offset %04x\n", address);
            switch (address) {
            case 0x1:  // PAR0-5
            case 0x2:
            case 0x3:
            case 0x4:
            case 0x5:
            case 0x6:
                ne2000->physaddr[address - 1] = value;
                break;

            case 0x7:  // CURR
                ne2000->curr_page = value;
                break;

            case 0x8:  // MAR0-7
            case 0x9:
            case 0xa:
            case 0xb:
            case 0xc:
            case 0xd:
            case 0xe:
            case 0xf:
                ne2000->mchash[address - 8] = value;
                break;
            }
            break;

        case 0x02:
            if (address != 0)
                pclog("page 2 write ?\n");

            switch (address) {
            case 0x1:  // CLDA0
              // Clear out low byte and re-insert
                ne2000->local_dma &= 0xff00;
                ne2000->local_dma |= (value & 0xff);
                break;

            case 0x2:  // CLDA1
              // Clear out high byte and re-insert
                ne2000->local_dma &= 0x00ff;
                ne2000->local_dma |= ((value & 0xff) << 8);
                break;

            case 0x3:  // Remote Next-pkt pointer
                ne2000->rempkt_ptr = value;
                break;

            case 0x4:
                //fatal("page 2 write to reserved offset 4\n");
                //OS/2 Warp can cause this to freak out.
                pclog("ne2000 page 2 write to reserved offset 4\n");
                break;

            case 0x5:  // Local Next-packet pointer
                ne2000->localpkt_ptr = value;
                break;

            case 0x6:  // Address counter (upper)
              // Clear out high byte and re-insert
                ne2000->address_cnt &= 0x00ff;
                ne2000->address_cnt |= ((value & 0xff) << 8);
                break;

            case 0x7:  // Address counter (lower)
              // Clear out low byte and re-insert
                ne2000->address_cnt &= 0xff00;
                ne2000->address_cnt |= (value & 0xff);
                break;

            case 0x8:
            case 0x9:
            case 0xa:
            case 0xb:
            case 0xc:
            case 0xd:
            case 0xe:
            case 0xf:
                //fatal("page 2 write to reserved offset %0x\n", address);
                pclog("ne2000 page 2 write to reserved offset %0x\n", address);
            default:
                break;
            }
            break;

        case 3:
            /*if (network_card_current == 1)
            {
              pclog("ne2000 unknown value of pgsel in write - %d\n", ne2000->CR.pgsel);
              break;
            }*/ //todo

            switch (address)
            {
            case 0:
                ne2000->cr = value;
                break;
            case 1:
                ne2000->i9346cr = value;
                break;
            case 5:
                if ((ne2000->i9346cr & 0xC0) == 0xC0)  ne2000->config2 = value;
                break;
            case 6:
                if ((ne2000->i9346cr & 0xC0) == 0xC0)  ne2000->config3 = value;
                break;
            case 9:
                ne2000->hltclk = value;
                break;
            }
            break;

        default:
            //fatal("ne2K: unknown value of pgsel in write - %d\n", ne2000->CR.pgsel);
            pclog("ne2000 unknown value of pgsel in write - %d\n", ne2000->CR.pgsel);
            break;
        }
    }
}

/*
 * mcast_index() - return the 6-bit index into the multicast
 * table. Stolen unashamedly from FreeBSD's if_ed.c
 */
static int mcast_index(const void* dst)
{
#define POLYNOMIAL 0x04c11db6
    unsigned long crc = 0xffffffffL;
    int carry, i, j;
    unsigned char b;
    unsigned char* ep = (unsigned char*)dst;

    for (i = 6; --i >= 0;) {
        b = *ep++;
        for (j = 8; --j >= 0;) {
            carry = ((crc & 0x80000000L) ? 1 : 0) ^ (b & 0x01);
            crc <<= 1;
            b >>= 1;
            if (carry)
                crc = ((crc ^ POLYNOMIAL) | carry);
        }
    }
    return (crc >> 26);
#undef POLYNOMIAL
}

/*
 * rx_frame() - called by the platform-specific code when an
 * ethernet frame has been received. The destination address
 * is tested to see if it should be accepted, and if the
 * rx ring has enough room, it is copied into it and
 * the receive process is updated
 */
void ne2000_rx_frame(void* p, const void* buf, int io_len)
{
    NE2000_t* ne2000 = (NE2000_t*)p;

    int pages;
    int avail;
    int idx;
    int wrapped;
    int nextpage;
    uint8_t pkthdr[4];
    uint8_t* pktbuf = (uint8_t*)buf;
    uint8_t* startptr;
    static uint8_t bcast_addr[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };

    /*if(io_len != 60) {
          pclog("rx_frame with length %d\n", io_len);
    }*/

    //LOG_MSG("stop=%d, pagestart=%x, dcr_loop=%x, tcr_loopcntl=%x",
    //      ne2000->CR.stop, ne2000->page_start,
    //      ne2000->DCR.loop, ne2000->TCR.loop_cntl);
    if ((ne2000->CR.stop != 0) ||
        (ne2000->page_start == 0) /*||
        ((ne2000->DCR.loop == 0) &&
         (ne2000->TCR.loop_cntl != 0))*/) {
        return;
    }

    // Add the pkt header + CRC to the length, and work
    // out how many 256-byte pages the frame would occupy
    pages = (io_len + 4 + 4 + 255) / 256;

    if (ne2000->curr_page < ne2000->bound_ptr) {
        avail = ne2000->bound_ptr - ne2000->curr_page;
    }
    else {
        avail = (ne2000->page_stop - ne2000->page_start) -
            (ne2000->curr_page - ne2000->bound_ptr);
        wrapped = 1;
    }

    // Avoid getting into a buffer overflow condition by not attempting
    // to do partial receives. The emulation to handle this condition
    // seems particularly painful.
    if ((avail < pages)
#if BX_NE2K_NEVER_FULL_RING
        || (avail == pages)
#endif
        ) {
        pclog("no space\n");
        return;
    }

    if ((io_len < 40/*60*/) && !ne2000->RCR.runts_ok) {
        pclog("rejected small packet, length %d\n", io_len);
        return;
    }
    // some computers don't care...
    if (io_len < 60) io_len = 60;

    // Do address filtering if not in promiscuous mode
    if (!ne2000->RCR.promisc) {
        if (!memcmp(buf, bcast_addr, 6)) {
            if (!ne2000->RCR.broadcast) {
                return;
            }
        }
        else if (pktbuf[0] & 0x01) {
            if (!ne2000->RCR.multicast) {
                return;
            }
            idx = mcast_index(buf);
            if (!(ne2000->mchash[idx >> 3] & (1 << (idx & 0x7)))) {
                return;
            }
        }
        else if (0 != memcmp(buf, ne2000->physaddr, 6)) {
            return;
        }
    }
    else {
        pclog("rx_frame promiscuous receive\n");
    }

    pclog("rx_frame %d to %x:%x:%x:%x:%x:%x from %x:%x:%x:%x:%x:%x\n",
        io_len,
        pktbuf[0], pktbuf[1], pktbuf[2], pktbuf[3], pktbuf[4], pktbuf[5],
        pktbuf[6], pktbuf[7], pktbuf[8], pktbuf[9], pktbuf[10], pktbuf[11]);

    {
        int i;
        for (i = 0; i < io_len; i++) {
            pclog("%02X ", pktbuf[i]);
        }
        pclog("\n");
    }

    nextpage = ne2000->curr_page + pages;
    if (nextpage >= ne2000->page_stop) {
        nextpage -= ne2000->page_stop - ne2000->page_start;
    }

    // Setup packet header
    pkthdr[0] = 0;        // rx status - old behavior
    pkthdr[0] = 1;        // Probably better to set it all the time
                          // rather than set it to 0, which is clearly wrong.
    if (pktbuf[0] & 0x01) {
        pkthdr[0] |= 0x20;  // rx status += multicast packet
    }
    pkthdr[1] = nextpage; // ptr to next packet
    pkthdr[2] = (io_len + 4) & 0xff;      // length-low
    pkthdr[3] = (io_len + 4) >> 8;        // length-hi

    // copy into buffer, update curpage, and signal interrupt if config'd
    startptr = &ne2000->mem[ne2000->curr_page * 256 -
        BX_NE2K_MEMSTART];
    if ((nextpage > ne2000->curr_page) ||
        ((ne2000->curr_page + pages) == ne2000->page_stop)) {
        memcpy(startptr, pkthdr, 4);
        memcpy(startptr + 4, buf, io_len);
        ne2000->curr_page = nextpage;
    }
    else {
        int endbytes = (ne2000->page_stop - ne2000->curr_page)
            * 256;
        memcpy(startptr, pkthdr, 4);
        memcpy(startptr + 4, buf, endbytes - 4);
        startptr = &ne2000->mem[ne2000->page_start * 256 -
            BX_NE2K_MEMSTART];
        memcpy(startptr, (void*)(pktbuf + endbytes - 4),
            io_len - endbytes + 8);
        ne2000->curr_page = nextpage;
    }

    ne2000->RSR.rx_ok = 1;
    if (pktbuf[0] & 0x80) {
        ne2000->RSR.rx_mbit = 1;
    }

    ne2000->ISR.pkt_rx = 1;

    if (ne2000->IMR.rx_inte) {
        //LOG_MSG("packet rx interrupt");
        pclog("packet rx interrupt\n");
        i8259_doirq(ne2000->i8259, ne2000->base_irq);
        //doirq(ne2000->base_irq);
        //picint(1 << ne2000->base_irq);
  //DEV_pic_raise_irq(ne2000->base_irq);
    } //else LOG_MSG("no packet rx interrupt");

}

void NE2000_tx_timer(void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;

    pclog("tx_timer\n");
    ne2000->TSR.tx_ok = 1;
    // Generate an interrupt if not masked and not one in progress
    if (ne2000->IMR.tx_inte && !ne2000->ISR.pkt_tx) {
        //LOG_MSG("tx complete interrupt");
        pclog("tx complete interrupt\n");
        i8259_doirq(ne2000->i8259, ne2000->base_irq);
        //doirq(ne2000->base_irq);
        //picint(1 << ne2000->base_irq);
  //DEV_pic_raise_irq(ne2000->base_irq);
    } //else        LOG_MSG("no tx complete interrupt");
    ne2000->ISR.pkt_tx = 1;
    ne2000->tx_timer_active = 0;
}

static void NE2000_tx_event(int val, void* p)
{
    NE2000_t* ne2000 = (NE2000_t*)p;
    NE2000_tx_timer(ne2000);
}

static void ne2000_poller(void* p) //todo
{
    NE2000_t* ne2000 = (NE2000_t*)p;
    struct queuepacket* qp;
    const unsigned char* data;
    //struct pcap_pkthdr h;


    int res;
    /*if(net_is_slirp) {
            while(QueuePeek(slirpq)>0)
                    {
                    qp=QueueDelete(slirpq);
                    if((ne2000->DCR.loop == 0) || (ne2000->TCR.loop_cntl != 0))
                            {
                            free(qp);
                            return;
                            }
                    ne2000_rx_frame(ne2000,&qp->data,qp->len);
                    pclog("ne2000 inQ:%d  got a %dbyte packet @%d\n",QueuePeek(slirpq),qp->len,qp);
                    free(qp);
                    }
            fizz++;
            if(fizz>1200){fizz=0;slirp_tic();}
            }//end slirp
    if(net_is_pcap  && net_pcap!=NULL)
            {
            data=_pcap_next(net_pcap,&h);
            if(data==0x0){goto WTF;}
            if((memcmp(data+6,maclocal,6))==0)
                pclog("ne2000 we just saw ourselves\n");
            else {
                 if((ne2000->DCR.loop == 0) || (ne2000->TCR.loop_cntl != 0))
                    {
                    return;
                    }
                    pclog("ne2000 pcap received a frame %d bytes\n",h.caplen);
                    ne2000_rx_frame(ne2000,data,h.caplen);
                 }
            WTF:
                    {}
            }*/
}


uint8_t ne2000_pci_regs[256];

void ne2000_io_set(uint16_t addr, NE2000_t* ne2000)
{
    /*io_sethandler(addr, 0x0010, ne2000_read, NULL, NULL, ne2000_write, NULL, NULL, ne2000);
    io_sethandler(addr+0x10, 0x0010, ne2000_asic_read_b, ne2000_asic_read_w, NULL, ne2000_asic_write_b, ne2000_asic_write_w, NULL, ne2000);
    io_sethandler(addr+0x1f, 0x0001, ne2000_reset_read, NULL, NULL, ne2000_reset_write, NULL, NULL, ne2000);*/
}

void ne2000_io_remove(uint16_t addr, NE2000_t* ne2000)
{
    /*    io_removehandler(addr, 0x0010, ne2000_read, NULL, NULL, ne2000_write, NULL, NULL, ne2000);
        io_removehandler(addr+0x10, 0x0010, ne2000_asic_read_b, ne2000_asic_read_w, NULL, ne2000_asic_write_b, ne2000_asic_write_w, NULL, ne2000);
        io_removehandler(addr+0x1f, 0x0001, ne2000_reset_read, NULL, NULL, ne2000_reset_write, NULL, NULL, ne2000);*/
}

void ne2000_isa_init()
{
    //disable_netbios = 1;
    //mem_mapping_disable(&netbios_mapping);
    //memset(netbios, 0, 32768);
    *(uint32_t*)&(ne2000_pci_regs[0x30]) = 0;
}


//BELOW IS ALL CODE TO INTERFACE WITH XTulator

uint8_t ne2000_portReadB(NE2000_t* ne2000, uint32_t addr) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port read byte at %03X\r\n", addr);
#endif
    return ne2000_read(addr, ne2000);
}

void ne2000_portWriteB(NE2000_t* ne2000, uint32_t addr, uint8_t value) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port write byte at %03X: %02X\r\n", addr, value);
#endif
    ne2000_write(addr, (uint16_t)value, ne2000);
}

uint8_t ne2000_portAsicReadB(NE2000_t* ne2000, uint32_t addr) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port ASIC read byte at %03X\r\n", addr);
#endif
    return ne2000_asic_read_b(addr, ne2000);
}

void ne2000_portAsicWriteB(NE2000_t* ne2000, uint32_t addr, uint8_t value) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port ASIC write byte at %03X: %02X\r\n", addr, value);
#endif
    ne2000_asic_write_b(addr, value, ne2000);
}

uint16_t ne2000_portAsicReadW(NE2000_t* ne2000, uint32_t addr) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port ASIC read word at %03X\r\n", addr);
#endif
    return ne2000_asic_read_w(addr, ne2000);
}

void ne2000_portAsicWriteW(NE2000_t* ne2000, uint32_t addr, uint16_t value) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port ASIC write word at %03X: %04X\r\n", addr, value);
#endif
    ne2000_asic_write_w(addr, value, ne2000);
}

uint8_t ne2000_portResetRead(NE2000_t* ne2000, uint32_t addr) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port reset read at %03X\r\n", addr);
#endif
    return ne2000_reset_read(addr, ne2000);
}

void ne2000_portResetWrite(NE2000_t* ne2000, uint32_t addr, uint8_t value) {
#ifdef DEBUG_NE2000
    debug_log(DEBUG_DETAIL, "[NE2000] Port reset write at %03X: %02X\r\n", addr, value);
#endif
    ne2000_reset_write(addr, (uint16_t)value, ne2000);
}

void ne2000_init(NE2000_t* ne2000, I8259_t* i8259, uint16_t baseport, uint8_t irq, uint8_t* macaddr) {
    debug_log(DEBUG_INFO, "[NE2000] Initializing NE2000 Ethernet adapter at 0x%03X, IRQ %u\r\n", baseport, irq);
    /*set_port_read_redirector(baseport, baseport + 0x0F, ne2k_read);
    set_port_write_redirector(baseport, baseport + 0x0F, ne2k_write);
    set_port_read_redirector(baseport + 0x10, baseport + 0x1E, ne2k_asic_read_b);
    set_port_write_redirector(baseport + 0x10, baseport + 0x1E, ne2k_asic_write_b);
    set_port_read_redirector(baseport + 0x1F, baseport + 0x1F, ne2k_reset_read);
    set_port_write_redirector(baseport + 0x1F, baseport + 0x1F, ne2k_reset_write);
    set_port_read_redirector_16(baseport + 0x10, baseport + 0x1F, ne2k_asic_read_w);
    set_port_write_redirector_16(baseport + 0x10, baseport + 0x1F, ne2k_asic_write_w);*/

    ne2000->i8259 = i8259;

    ports_cbRegister(baseport, 16, ne2000_portReadB, NULL, ne2000_portWriteB, NULL, ne2000);
    ports_cbRegister(baseport + 0x10, 15, ne2000_portAsicReadB, ne2000_portAsicReadW, ne2000_portAsicWriteB, ne2000_portAsicWriteW, ne2000);
    ports_cbRegister(baseport + 0x1F, 1, ne2000_portResetRead, NULL, ne2000_portResetWrite, NULL, ne2000);

    ne2000_io_set(baseport, ne2000);
    ne2000_setirq(ne2000, irq);
    memcpy(ne2000->physaddr, macaddr, 6);
    ne2000_isa_init();
    ne2000_reset(BX_RESET_HARDWARE, ne2000);
}

#endif