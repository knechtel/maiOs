#include "e1000.h"

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC
#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E

#define REG_CTRL 0x0000
#define REG_ICR  0x00C0
#define REG_IMC  0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RAL0 0x5400
#define REG_RAH0 0x5404
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818

#define CTRL_RST (1u << 26)
#define CTRL_SLU (1u << 6)
#define RX_COUNT 8
#define TX_COUNT 8
#define RX_BUFFER_SIZE 2048
#define MAX_FRAME_SIZE 1518
#define RX_STATUS_DD 0x01
#define RX_STATUS_EOP 0x02
#define TX_STATUS_DD 0x01
#define TX_CMD_EOP 0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS 0x08

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} rx_descriptor_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} tx_descriptor_t;

_Static_assert(sizeof(rx_descriptor_t) == 16, "E1000 RX descriptors are 16 bytes");
_Static_assert(sizeof(tx_descriptor_t) == 16, "E1000 TX descriptors are 16 bytes");

static rx_descriptor_t rx_ring[RX_COUNT] __attribute__((aligned(128)));
static tx_descriptor_t tx_ring[TX_COUNT] __attribute__((aligned(128)));
static uint8_t rx_buffers[RX_COUNT][RX_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[TX_COUNT][MAX_FRAME_SIZE] __attribute__((aligned(16)));
static volatile uint32_t *e1000_regs;
static uint8_t rx_index, tx_index, e1000_ready;

static void bytes_copy(uint8_t *to, const uint8_t *from, uint16_t length)
{
    for (uint16_t i = 0; i < length; ++i) to[i] = from[i];
}

static uint32_t dma_address(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static void compiler_barrier(void)
{
    __asm__ volatile ("" : : : "memory");
}

static void setup_rings(void)
{
    for (uint32_t i = 0; i < RX_COUNT; ++i) {
        rx_ring[i].address = dma_address(rx_buffers[i]);
        rx_ring[i].length = 0;
        rx_ring[i].checksum = 0;
        rx_ring[i].status = 0;
        rx_ring[i].errors = 0;
        rx_ring[i].special = 0;
    }
    for (uint32_t i = 0; i < TX_COUNT; ++i) {
        tx_ring[i].address = dma_address(tx_buffers[i]);
        tx_ring[i].length = 0;
        tx_ring[i].checksum_offset = 0;
        tx_ring[i].command = 0;
        tx_ring[i].status = TX_STATUS_DD;
        tx_ring[i].checksum_start = 0;
        tx_ring[i].special = 0;
    }
    compiler_barrier();

    uint64_t rx_address = (uint64_t)(uintptr_t)rx_ring;
    e1000_regs[REG_RDBAL / 4] = (uint32_t)rx_address;
    e1000_regs[REG_RDBAH / 4] = (uint32_t)(rx_address >> 32);
    e1000_regs[REG_RDLEN / 4] = sizeof(rx_ring);
    e1000_regs[REG_RDH / 4] = 0;
    e1000_regs[REG_RDT / 4] = RX_COUNT - 1;

    uint64_t tx_address = (uint64_t)(uintptr_t)tx_ring;
    e1000_regs[REG_TDBAL / 4] = (uint32_t)tx_address;
    e1000_regs[REG_TDBAH / 4] = (uint32_t)(tx_address >> 32);
    e1000_regs[REG_TDLEN / 4] = sizeof(tx_ring);
    e1000_regs[REG_TDH / 4] = 0;
    e1000_regs[REG_TDT / 4] = 0;

    e1000_regs[REG_TIPG / 4] = 0x0060200A;
    e1000_regs[REG_TCTL / 4] = 0x000401FA; /* EN, pad short frames, collision settings */
    e1000_regs[REG_RCTL / 4] = (1u << 1) | (1u << 15) | (1u << 26); /* EN, broadcast, strip CRC */
    rx_index = 0;
    tx_index = 0;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint32_t value)
{
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static int find_e1000(uint8_t *bus_out, uint8_t *device_out, uint8_t *function_out)
{
    for (uint16_t device = 0; device < 32; ++device) {
        uint32_t id0 = pci_read32(0, (uint8_t)device, 0, 0);
        uint16_t vendor = (uint16_t)id0;
        if (vendor == 0xFFFF) continue;

        uint8_t header = (uint8_t)(pci_read32(0, (uint8_t)device, 0, 0x0C) >> 16);
        uint8_t functions = (header & 0x80) ? 8 : 1;
        for (uint8_t function = 0; function < functions; ++function) {
            uint32_t id = function == 0 ? id0 : pci_read32(0, (uint8_t)device, function, 0);
            if ((uint16_t)id == E1000_VENDOR_ID && (uint16_t)(id >> 16) == E1000_DEVICE_ID) {
                *bus_out = 0;
                *device_out = (uint8_t)device;
                *function_out = function;
                return 1;
            }
        }
    }
    return 0;
}

static int wait_reset(volatile uint32_t *registers)
{
    for (uint32_t i = 0; i < 1000000; ++i)
        if (!(registers[REG_CTRL / 4] & CTRL_RST)) return 1;
    return 0;
}

int e1000_init(uint8_t mac[6])
{
    e1000_ready = 0;
    uint8_t bus, device, function;
    if (!find_e1000(&bus, &device, &function)) return 0;

    uint32_t bar0 = pci_read32(bus, device, function, 0x10);
    if (bar0 == 0xFFFFFFFF || (bar0 & 1)) return 0; /* Require a memory BAR. */
    uint32_t bar_type = (bar0 >> 1) & 3;
    uint32_t mmio_base = bar0 & ~0xFu;
    if (bar_type == 2) {
        uint32_t high = pci_read32(bus, device, function, 0x14);
        if (high) return 0; /* This 32-bit kernel cannot map a BAR above 4 GiB. */
    } else if (bar_type != 0) {
        return 0;
    }
    if (!mmio_base) return 0;

    /* Enable memory-space access and PCI bus mastering, preserving other bits. */
    uint16_t command = (uint16_t)pci_read32(bus, device, function, 0x04);
    command |= 0x0006u;
    pci_write32(bus, device, function, 0x04, command);

    volatile uint32_t *registers = (volatile uint32_t *)(uintptr_t)mmio_base;
    registers[REG_IMC / 4] = 0xFFFFFFFFu;
    (void)registers[REG_ICR / 4];
    registers[REG_RCTL / 4] = 0;
    registers[REG_TCTL / 4] = 0;

    registers[REG_CTRL / 4] |= CTRL_RST;
    if (!wait_reset(registers)) return 0;
    registers[REG_IMC / 4] = 0xFFFFFFFFu;
    (void)registers[REG_ICR / 4];
    registers[REG_CTRL / 4] |= CTRL_SLU;

    uint32_t low = registers[REG_RAL0 / 4];
    uint32_t high = registers[REG_RAH0 / 4];
    mac[0] = (uint8_t)low;
    mac[1] = (uint8_t)(low >> 8);
    mac[2] = (uint8_t)(low >> 16);
    mac[3] = (uint8_t)(low >> 24);
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);

    uint8_t any = 0;
    for (uint8_t i = 0; i < 6; ++i) any |= mac[i];
    if (!any || !(high & 0x80000000u)) return 0;

    e1000_regs = registers;
    setup_rings();
    e1000_ready = 1;
    return 1;
}

int e1000_send(const uint8_t *frame, uint16_t length)
{
    if (!e1000_ready || !frame || length < 14 || length > MAX_FRAME_SIZE) return -1;
    tx_descriptor_t *desc = &tx_ring[tx_index];
    for (uint32_t wait = 0; wait < 10000000; ++wait) {
        compiler_barrier();
        if (desc->status & TX_STATUS_DD) break;
        if (wait == 9999999) return 0;
    }

    bytes_copy(tx_buffers[tx_index], frame, length);
    desc->length = length;
    desc->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    desc->status = 0;
    compiler_barrier();
    tx_index = (uint8_t)((tx_index + 1) % TX_COUNT);
    e1000_regs[REG_TDT / 4] = tx_index;
    return 1;
}

int e1000_receive(uint8_t *frame, uint16_t capacity)
{
    if (!e1000_ready || !frame) return -1;
    rx_descriptor_t *desc = &rx_ring[rx_index];
    compiler_barrier();
    if (!(desc->status & RX_STATUS_DD)) return 0;

    uint16_t length = desc->length;
    int valid = (desc->status & RX_STATUS_EOP) && !desc->errors &&
                length >= 14 && length <= RX_BUFFER_SIZE && length <= capacity;
    if (valid) bytes_copy(frame, rx_buffers[rx_index], length);

    desc->length = 0;
    desc->checksum = 0;
    desc->status = 0;
    desc->errors = 0;
    desc->special = 0;
    compiler_barrier();
    e1000_regs[REG_RDT / 4] = rx_index;
    rx_index = (uint8_t)((rx_index + 1) % RX_COUNT);
    return valid ? length : -1;
}
