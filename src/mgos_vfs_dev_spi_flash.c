/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos_vfs_dev_spi_flash.h"
#include "mgos_spi.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "common/cs_dbg.h"
#include "common/platform.h"

#include "frozen.h"

#include "mgos_hal.h"
#include "mgos_utils.h"
#include "mgos_vfs_dev.h"

#define SPI_FLASH_OP_PROGRAM_PAGE 0x02
#define SPI_FLASH_OP_WRDI 0x04
#define SPI_FLASH_OP_RDSR 0x05
#define SPI_FLASH_OP_WREN 0x06
#define SPI_FLASH_OP_READ_FAST 0x0b
#define SPI_FLASH_OP_ERASE_SECTOR 0x20
#define SPI_FLASH_OP_READ_SFDP 0x5a
#define SPI_FLASH_OP_READ_JEDEC_ID 0x9f

#define SPI_FLASH_PAGE_SIZE 0x100
#define SPI_FLASH_SECTOR_SIZE 0x1000
#define SPI_FLASH_DEFAULT_WIP_MASK 0x01

#define SFDP_MAGIC 0x50444653     /* "SFDP" (LE) */
#define SFDP_V10_PT0_LEN (9 * 4)  /* rev 1.0 */
#define SFDP_V16_PT0_LEN (16 * 4) /* rev 1.6 */

/* Note: structs below assume LE architecture */

/* SFDP is documented in JESD216 */
struct __attribute__((packed)) sfdp_header {
  uint32_t magic;
  struct {
    uint32_t minor_rev : 8;
    uint32_t major_rev : 8;
    uint32_t nph : 8;
    uint32_t unused_ff : 8;
  };
};

struct __attribute__((packed)) sfdp_parameter_header {
  struct {
    uint32_t mfg_id : 8;
    uint32_t minor_rev : 8;
    uint32_t major_rev : 8;
    uint32_t len_dw : 8;
  };
  struct {
    uint32_t addr : 24;
    uint32_t unused_ff : 8;
  };
};

struct __attribute__((packed)) sfdp_pt0 {
  struct {                    /* DW 0 */
    uint32_t erase_size : 2;  /* 01: 4K; 00,10: rsvd; 11: no 4K erase. */
    uint32_t write_gran : 1;  /* Write granularity: 0: < 64bytes; 1: >= 64 */
    uint32_t wren_vsr : 1;    /* 1: WREN required to write VSR */
    uint32_t wren_vsr_op : 1; /* 0: 0x50 to write to VSR; 1: 0x06 */
    uint32_t unused_111 : 3;  /* Unused, contain 111. */
    uint32_t erase_4k_op : 8; /* Opcode to use for 4K erase. */
    uint32_t fr_1_1_2 : 1;    /* Supports 1-1-2 Fast Read. */
    uint32_t addr_bytes : 2;  /* 00: 3 only; 01: 3 or 4; 10: 4 only; 11: rsvd */
    uint32_t dtr : 1;         /* 1: Supportes Double Transfer Rate (DTR) */
    uint32_t fr_1_2_2 : 1;    /* 1: 1-2-2 Fast Read is supported. */
    uint32_t fr_1_4_4 : 1;    /* 1: 1-4-4 Fast Read is supported. */
    uint32_t fr_1_1_4 : 1;    /* 1: 1-1-4 Fast Read is supported. */
    uint32_t unused_1 : 1;    /* Unused, set to 1. */
    uint32_t unused_ff : 8;   /* Unused, contains 0xff. */
  };
  struct {                 /* DW 1 */
    uint32_t density : 31; /* Density value */
    uint32_t over_2g : 1;  /* 0: density is # of bits; 1: density is 2^N */
  };
  struct {                     /* DW 2 */
    uint32_t fr_1_4_4_nws : 5; /* Number of dummy cycles for 1-4-4 read */
    uint32_t fr_1_4_4_nmb : 3; /* Number of mode bits for 1-4-4 read */
    uint32_t fr_1_4_4_op : 8;  /* 1-4-4 read opcode */
    uint32_t fr_1_1_4_nws : 5; /* Number of dummy cycles for 1-1-4 read */
    uint32_t fr_1_1_4_nmb : 3; /* Number of mode bits for 1-1-4 read */
    uint32_t fr_1_1_4_op : 8;  /* 1-1-4 read opcode */
  };
  struct {                     /* DW 3 */
    uint32_t fr_1_1_2_nws : 5; /* Number of dummy cycles for 1-1-2 read */
    uint32_t fr_1_1_2_nmb : 3; /* Number of mode bits for 1-1-2 read */
    uint32_t fr_1_1_2_op : 8;  /* 1-1-2 read opcode */
    uint32_t fr_1_2_2_nws : 5; /* Number of dummy cycles for 1-2-2 read */
    uint32_t fr_1_2_2_nmb : 3; /* Number of mode bits for 1-2-2 read */
    uint32_t fr_1_2_2_op : 8;  /* 1-2-2 read opcode */
  };
  struct {                  /* DW 4 */
    uint32_t fr_2_2_2 : 1;  /* 1: 2-2-2 Fast Read is supported */
    uint32_t rsvd_4_1 : 3;  /* Reserved, set to 111. */
    uint32_t fr_4_4_4 : 1;  /* 1: 4-4-4 Fast Read is supported */
    uint32_t rsvd_4_2 : 27; /* Reserved, set to all 1s. */
  };
  struct {                     /* DW 5 */
    uint32_t rsvd_5 : 16;      /* Reserved, set to all 1s. */
    uint32_t fr_2_2_2_nws : 5; /* Number of dummy cycles for 2-2-2 read */
    uint32_t fr_2_2_2_nmb : 3; /* Number of mode bits for 2-2-2 read */
    uint32_t fr_2_2_2_op : 8;  /* 2-2-2 read opcode */
  };
  struct {                     /* DW 6 */
    uint32_t rsvd_6 : 16;      /* Reserved, set to all 1s. */
    uint32_t fr_4_4_4_nws : 5; /* Number of dummy cycles for 4-4-4 read */
    uint32_t fr_4_4_4_nmb : 3; /* Number of mode bits for 4-4-4 read */
    uint32_t fr_4_4_4_op : 8;  /* 4-4-4 read opcode */
  };
  struct {                       /* DW 7 */
    uint32_t erase_st1_size : 8; /* Erase sector type 1 size (2^N) */
    uint32_t erase_st1_op : 8;   /* Erase sector type 1 opcode */
    uint32_t erase_st2_size : 8; /* Erase sector type 2 size (2^N) */
    uint32_t erase_st2_op : 8;   /* Erase sector type 2 opcode */
  };
  struct {                       /* DW 8 */
    uint32_t erase_st3_size : 8; /* Erase sector type 3 size (2^N) */
    uint32_t erase_st3_op : 8;   /* Erase sector type 3 opcode */
    uint32_t erase_st4_size : 8; /* Erase sector type 4 size (2^N) */
    uint32_t erase_st4_op : 8;   /* Erase sector type 4 opcode */
  };
  /* Fields added in SFDP 1.6 fields (JESD216A and B) */
  struct {                        /* DW 9 */
    uint32_t erase_max_mul : 4;   /* Multiplier from typ to max erase time */
    uint32_t erase_st1_typ : 5;   /* Sector type 1 typical erase time */
    uint32_t erase_st1_typ_u : 2; /* Sector type 1 typical erase time (units) */
    uint32_t erase_st2_typ : 5;   /* Sector type 2 typical erase time */
    uint32_t erase_st2_typ_u : 2; /* Sector type 2 typical erase time (units) */
    uint32_t erase_st3_typ : 5;   /* Sector type 3 typical erase time */
    uint32_t erase_st3_typ_u : 2; /* Sector type 3 typical erase time (units) */
    uint32_t erase_st4_typ : 5;   /* Sector type 4 typical erase time */
    uint32_t erase_st4_typ_u : 2; /* Sector type 4 typical erase time (units) */
  };
  struct {                         /* DW 10 */
    uint32_t prog_max_mul : 4;     /* Multiplier from typ to max prog time */
    uint32_t prog_page_size : 4;   /* Page size (2^N) */
    uint32_t prog_page_typ : 5;    /* Typ page prog time */
    uint32_t prog_page_typ_u : 1;  /* Typ page prog time (units) */
    uint32_t prog_byte1_typ : 4;   /* Typ byte prog time, 1st byte */
    uint32_t prog_byte1_typ_u : 1; /* Typ byte prog time, 1st byte (units) */
    uint32_t prog_byte_typ : 4;    /* Typ byte prog time, 2nd+ byte */
    uint32_t prog_byte_typ_u : 1;  /* Typ byte prog time, 2nd+ byte (units) */
    uint32_t erase_chip_typ : 5;   /* Typ chip erase time */
    uint32_t erase_chip_typ_u : 2; /* Typ chip erase time (units) */
    uint32_t rsvd_10 : 1;          /* Reserved */
  };
  struct {                              /* DW 11 */
    uint32_t prog_susp_inv_ops : 4;     /* Ops invalid during prog suspend */
    uint32_t erase_susp_inv_ops : 4;    /* Ops invalid during erase suspend */
    uint32_t rsvd_11 : 1;               /* Reserved */
    uint32_t prog_res_to_susp_int : 4;  /* Prog resume to suspend interval */
    uint32_t prog_susp_max_lat : 5;     /* Suspend prog, max latency */
    uint32_t prog_susp_max_lat_u : 2;   /* Suspend prog, max latency (units) */
    uint32_t erase_res_to_susp_int : 4; /* Erase resume to suspend interval */
    uint32_t erase_susp_max_lat : 5;    /* Suspend erase, max latency */
    uint32_t erase_susp_max_lat_u : 2;  /* Suspend erase, max latency (units) */
    uint32_t susp_resume_unsupp : 1;    /* Suspend/resume unsupp (0 = supp) */
  };
  struct {                        /* DW 12 */
    uint32_t prog_resume_op : 8;  /* Program resume opcode */
    uint32_t prog_suspend_op : 8; /* Program suspend opcode */
    uint32_t resume_op : 8;       /* Resume opcode */
    uint32_t suspend_op : 8;      /* Suspend opcode */
  };
  struct {                        /* DW 13 */
    uint32_t rsvd_13 : 2;         /* Reserved */
    uint32_t srp_busy : 6;        /* Status register polling device busy */
    uint32_t dpd_exit_time : 5;   /* Return from deep power down time */
    uint32_t dpd_exit_time_u : 2; /* Return from deep power down time (units) */
    uint32_t dpd_exit_op : 8;     /* Exit deep power down op */
    uint32_t dpd_enter_op : 8;    /* Enter deep power down op */
    uint32_t dpd_unsupp : 1;      /* Deep power down unsupp (0 = supp) */
  };
  struct {                        /* DW 14 */
    uint32_t m_4_4_4_dis_seq : 4; /* 4-4-4 mode disable sequence */
    uint32_t m_4_4_4_en_seq : 5;  /* 4-4-4 mode enable sequence */
    uint32_t m_0_4_4_sup : 1;     /* 0-4-4 mode supported */
    uint32_t m_0_4_4_dis : 6;     /* 0-4-4 mode disable sequence */
    uint32_t m_0_4_4_en : 4;      /* 0-4-4 mode enable sequence */
    uint32_t quad_en_req : 3;     /* Quad enable requirements */
    uint32_t hold_wp_dis : 1;     /* Hold and WP disable */
    uint32_t rsvd_14 : 8;         /* Reserved */
  };
  struct {                    /* DW 15 */
    uint32_t wren_nv_reg : 7; /* WREN and volatile/nonvolatile registers */
    uint32_t rsvd_15 : 1;     /* Reserved */
    uint32_t sfrr_sup : 6;    /* Soft reset and rescue sequence support */
    uint32_t exit_4ba : 10;   /* Exit 4 byte address mode */
    uint32_t enter_4ba : 8;   /* Enter 4 byte address mode */
  };
};

static uint32_t sfdp_sector_erase_time_ms(uint8_t val, uint8_t unit) {
  uint32_t v32 = ((uint32_t) val) + 1;
  switch (unit) {
    case 0:
      return v32;
    case 1:
      return v32 * 16;
    case 2:
      return v32 * 128;
    case 3:
      return v32 * 1000;
  }
  return 0;
}

static uint32_t sfdp_chip_erase_time_ms(uint8_t val, uint8_t unit) {
  uint32_t v32 = ((uint32_t) val) + 1;
  switch (unit) {
    case 0:
      return v32 * 16;
    case 1:
      return v32 * 256;
    case 2:
      return v32 * 4000;
    case 3:
      return v32 * 64000;
  }
  return 0;
}

static uint32_t sfdp_dpd_exit_time_us(uint8_t val, uint8_t unit) {
  uint32_t v32 = ((uint32_t) val) + 1;
  switch (unit) {
    case 0:
      return v32 / 8;
    case 1:
      return v32;
    case 2:
      return v32 * 8;
    case 3:
      return v32 * 64;
  }
  return 0;
}

#ifndef htonl
static uint32_t swap_byte_32(uint32_t x) {
  return (((x & 0x000000ffUL) << 24) | ((x & 0x0000ff00UL) << 8) |
          ((x & 0x00ff0000UL) >> 8) | ((x & 0xff000000UL) >> 24));
}
#define htonl(x) swap_byte_32(x)
#endif /* htonl */

struct dev_data {
  struct mgos_spi *spi;
  int cs;
  int freq;
  int mode;
  size_t size;
  uint8_t read_op;
  uint8_t read_op_nwb;
  uint8_t erase_sector_op;
  uint8_t wip_mask;
  uint8_t dpd_enter_op, dpd_exit_op;
  uint32_t dpd_en : 1; /* Enable Deep Power Down when inactive (if supported) */
  uint32_t dpd_on : 1; /* The chip is currently in Deep Power Down mode */
  uint32_t dpd_exit_sleep_us : 8;
};

static bool spi_flash_op(struct dev_data *dd, size_t tx_len,
                         const void *tx_data, int dummy_len, size_t rx_len,
                         void *rx_data) {
  struct mgos_spi_txn txn = {.cs = dd->cs, .mode = dd->mode, .freq = dd->freq};
  txn.hd.tx_len = tx_len;
  txn.hd.tx_data = tx_data;
  txn.hd.dummy_len = dummy_len;
  txn.hd.rx_len = rx_len;
  txn.hd.rx_data = rx_data;
  return mgos_spi_run_txn(dd->spi, false /* fd */, &txn);
}

static bool spi_flash_simple_op(struct dev_data *dd, uint8_t op, int dummy_len,
                                size_t rx_len, void *rx_data) {
  return spi_flash_op(dd, 1, &op, dummy_len, rx_len, rx_data);
}

static uint8_t spi_flash_rdsr(struct dev_data *dd) {
  uint8_t res = 0;
  spi_flash_simple_op(dd, SPI_FLASH_OP_RDSR, 0, 1, &res);
  return res;
}

static bool spi_flash_wren(struct dev_data *dd) {
  while ((spi_flash_rdsr(dd) & 2) == 0) {  // WEL
    if (!spi_flash_simple_op(dd, SPI_FLASH_OP_WREN, 0, 0, NULL)) return false;
  }
  return true;
}

static void spi_flash_dpd_enter(struct dev_data *dd) {
  if (!dd->dpd_en) return;
  spi_flash_simple_op(dd, dd->dpd_enter_op, 0, 0, NULL);
  mgos_usleep(dd->dpd_exit_sleep_us / 4);
  dd->dpd_on = true;
}

static bool spi_flash_dpd_exit(struct dev_data *dd) {
  if (!dd->dpd_on) return true;
  spi_flash_simple_op(dd, dd->dpd_exit_op, 0, 0, NULL);
  mgos_usleep(dd->dpd_exit_sleep_us);
  dd->dpd_on = false;
  return true;
}

static bool spi_flash_wait_idle(struct dev_data *dd) {
  uint8_t st;
  do {
    st = spi_flash_rdsr(dd);
  } while (st & dd->wip_mask);
  /* TODO(rojer): feed WDT and timeout */
  return true;
}

/* Use SFDP to detect flash params. */
static bool mgos_vfs_dev_spi_flash_detect(struct dev_data *dd) {
  bool ret = false;
  uint32_t pt0_addr = 0;
  size_t sfdp_pt0_len = 0;

  /* Start with conservative settings */
  dd->read_op = SPI_FLASH_OP_READ_FAST;
  dd->read_op_nwb = 1;

  uint8_t jid[4] = {0, 0, 0, 0};
  if (!spi_flash_simple_op(dd, SPI_FLASH_OP_READ_JEDEC_ID, 0, sizeof(jid),
                           jid)) {
    LOG(LL_ERROR, ("Failed to read JEDEC ID"));
    goto out_err;
  }
  if (jid[0] == 0 || jid[0] == 0xff) {
    /* Some chips insert a dummy byte. */
    if (jid[3] != 0 && jid[3] != 0xff) {
      memmove(jid, jid + 1, 3);
    } else {
      LOG(LL_ERROR, ("Invalid JEDEC ID"));
      goto out_err;
    }
  }
  LOG(LL_DEBUG, ("JEDEC ID: %02x %02x %02x", jid[0], jid[1], jid[2]));

  { /* Retrieve SFDP header and parameter table pointer. */
    uint32_t tx_data = htonl(SPI_FLASH_OP_READ_SFDP << 24);
    uint32_t rx_data[4];
    if (!spi_flash_op(dd, 4, &tx_data, 1, sizeof(rx_data), rx_data)) {
      LOG(LL_DEBUG, ("Failed to read SFDP info"));
      goto out_nosfdp;
    }
    struct sfdp_header *sh = (struct sfdp_header *) rx_data;
    if (sh->magic != SFDP_MAGIC) {
      LOG(LL_DEBUG, ("Invalid SFDP magic (got 0x%08x)", (unsigned) sh->magic));
      goto out_nosfdp;
    }
    struct sfdp_parameter_header *pt0h =
        (struct sfdp_parameter_header *) &rx_data[2];
    sfdp_pt0_len = MIN(pt0h->len_dw * 4, sizeof(struct sfdp_pt0));
    LOG(LL_DEBUG,
        ("SFDP %u.%u len %u", sh->major_rev, sh->minor_rev, pt0h->len_dw * 4));
    if (sfdp_pt0_len < SFDP_V10_PT0_LEN) {
      LOG(LL_ERROR, ("Invalid SFDP PT0 length (%d)", (int) pt0h->len_dw));
      goto out_nosfdp;
    }
    pt0_addr = pt0h->addr;
  }
  { /* Get Parameter Table 0. */
    uint32_t tx_data = htonl((SPI_FLASH_OP_READ_SFDP << 24) | pt0_addr);
    struct sfdp_pt0 pt0;
    memset(&pt0, 0, sizeof(pt0));
    if (!spi_flash_op(dd, 4, &tx_data, 1, MIN(sfdp_pt0_len, sizeof(pt0)),
                      &pt0)) {
      LOG(LL_ERROR, ("Failed to read SFDP params"));
      goto out_nosfdp;
    }
    if (dd->size == 0) {
      dd->size = (pt0.over_2g ? (1 << pt0.density) : (pt0.density + 1)) / 8;
    }
    LOG(LL_DEBUG,
        ("Read modes :%s%s%s%s%s%s", (pt0.fr_1_1_2 ? " 1-1-2" : ""),
         (pt0.fr_1_2_2 ? " 1-2-2" : ""), (pt0.fr_1_4_4 ? " 1-4-4" : ""),
         (pt0.fr_1_1_4 ? " 1-1-4" : ""), (pt0.fr_2_2_2 ? " 2-2-2" : ""),
         (pt0.fr_4_4_4 ? " 4-4-4" : "")));
    LOG(LL_DEBUG,
        ("Erase: 0x%x:%d,%dms 0x%x:%d,%dms 0x%x:%d,%dms 0x%x:%d,%dms chip:%dms",
         (int) pt0.erase_st1_op,
         (int) (pt0.erase_st1_size > 0 ? 1 << pt0.erase_st1_size : 0),
         (int) sfdp_sector_erase_time_ms(pt0.erase_st1_typ,
                                         pt0.erase_st1_typ_u),
         (int) pt0.erase_st2_op,
         (int) (pt0.erase_st2_size > 0 ? 1 << pt0.erase_st2_size : 0),
         (int) sfdp_sector_erase_time_ms(pt0.erase_st2_typ,
                                         pt0.erase_st2_typ_u),
         (int) pt0.erase_st3_op,
         (int) (pt0.erase_st3_size > 0 ? 1 << pt0.erase_st3_size : 0),
         (int) sfdp_sector_erase_time_ms(pt0.erase_st3_typ,
                                         pt0.erase_st3_typ_u),
         (int) pt0.erase_st4_op,
         (int) (pt0.erase_st4_size > 0 ? 1 << pt0.erase_st4_size : 0),
         (int) sfdp_sector_erase_time_ms(pt0.erase_st4_typ,
                                         pt0.erase_st4_typ_u),
         (int) sfdp_chip_erase_time_ms(pt0.erase_chip_typ,
                                       pt0.erase_chip_typ_u)));
    dd->erase_sector_op = pt0.erase_4k_op;
    if (sfdp_pt0_len >= SFDP_V16_PT0_LEN) {
      dd->dpd_enter_op = pt0.dpd_enter_op;
      dd->dpd_exit_op = pt0.dpd_exit_op;
      dd->dpd_exit_sleep_us =
          sfdp_dpd_exit_time_us(pt0.dpd_exit_time, pt0.dpd_exit_time_u);
      if (dd->dpd_en) dd->dpd_en = !pt0.dpd_unsupp;
    } else {
      dd->dpd_en = false;
    }
    /* TODO(rojer): double and quad reads, if both chip and SPI support them. */
  }

out_nosfdp:
  /* Ok, we don't have SFDP. Let's see if JEDEC ID byte 2 looks like size. */
  if (dd->size == 0) {
    if (jid[2] > 10 && jid[2] <= 31) {
      dd->size = (1 << jid[2]);
    } else {
      LOG(LL_ERROR, ("Size not specified and could not be detected"));
      goto out_err;
    }
  }
  LOG(LL_DEBUG,
      ("Chip ID: %02x %02x, size: %d", jid[0], jid[1], (int) dd->size));
  ret = true;

out_err:
  return ret;
}

static bool mgos_vfs_dev_spi_flash_open(struct mgos_vfs_dev *dev,
                                        const char *opts) {
  bool ret = false;
  int dpd_en = 0;
  unsigned int wip_mask = SPI_FLASH_DEFAULT_WIP_MASK;
  struct dev_data *dd = (struct dev_data *) calloc(1, sizeof(*dd));
  if (dd == NULL) goto out;
  dd->spi = mgos_spi_get_global();
  if (dd->spi == NULL) {
    LOG(LL_INFO, ("SPI is disabled"));
    goto out;
  }
  dd->cs = -1;
  json_scanf(opts, strlen(opts),
             "{cs: %d, freq: %d, mode: %d, size: %d, wip_mask: %u, dpd: %B}",
             &dd->cs, &dd->freq, &dd->mode, &dd->size, &wip_mask, &dpd_en);
  if (dd->freq <= 0) goto out;
  dd->wip_mask = wip_mask;
  dd->dpd_en = dpd_en;
  if (dd->dpd_en) {
    /* So, the problem is that the device may already be in DPD, we need to take
     * it out of it.
     * But we don't know the op or delay yet! Well, we'll have to guess. */
    dd->dpd_on = true;
    dd->dpd_exit_op = 0xab;
    dd->dpd_exit_sleep_us = 100;
    spi_flash_dpd_exit(dd);
  }
  if (!mgos_vfs_dev_spi_flash_detect(dd)) goto out;
  if (dd->dpd_en && dd->dpd_enter_op == 0) dd->dpd_en = false;
  LOG(LL_DEBUG, ("DPD: %s, 0x%02x/0x%02x, %dus", (dd->dpd_en ? "yes" : "no"),
                 dd->dpd_enter_op, dd->dpd_exit_op, dd->dpd_exit_sleep_us));
  dev->dev_data = dd;
  spi_flash_dpd_enter(dd);
  ret = true;

out:
  if (!ret) free(dd);
  return ret;
}

static bool mgos_vfs_dev_spi_flash_read(struct mgos_vfs_dev *dev, size_t offset,
                                        size_t len, void *dst) {
  bool ret = true;
  struct dev_data *dd = (struct dev_data *) dev->dev_data;
  if (offset + len > dd->size) ret = false;
  if (ret && len > 0) {
    uint32_t tx_data = htonl((dd->read_op << 24) | offset);
    ret = ret && spi_flash_dpd_exit(dd);
    ret = ret && spi_flash_wait_idle(dd);
    ret = ret && spi_flash_op(dd, 4, &tx_data, dd->read_op_nwb, len, dst);
    spi_flash_dpd_enter(dd);
  }
  LOG(LL_VERBOSE_DEBUG, ("%p read %u @ 0x%x -> %d", dev, (unsigned int) len,
                         (unsigned int) offset, ret));
  return ret;
}

static bool mgos_vfs_dev_spi_flash_write(struct mgos_vfs_dev *dev,
                                         size_t offset, size_t len,
                                         const void *src) {
  bool ret = true;
  struct dev_data *dd = (struct dev_data *) dev->dev_data;
  if (offset + len > dd->size) ret = false;
  ret = ret && spi_flash_dpd_exit(dd);
  ret = ret && spi_flash_wait_idle(dd);
  const uint8_t *data = (const uint8_t *) src;
  size_t off = offset, l = len;
  while (ret && l > 0) {
    uint32_t tx_data[9] = {htonl((SPI_FLASH_OP_PROGRAM_PAGE << 24) | off)};
    size_t write_len = MIN(l, 32);
    if (off % SPI_FLASH_PAGE_SIZE != 0) {
      size_t page_start = (off & ~(SPI_FLASH_PAGE_SIZE - 1));
      size_t page_remain = (SPI_FLASH_PAGE_SIZE - (off - page_start));
      write_len = MIN(write_len, page_remain);
    }
    memcpy(tx_data + 1, data, write_len);
    ret = ret && spi_flash_wren(dd);
    ret = ret && spi_flash_op(dd, 4 + write_len, &tx_data, 0, 0, NULL);
    ret = ret && spi_flash_wait_idle(dd);
    l -= write_len;
    data += write_len;
    off += write_len;
  }
  spi_flash_dpd_enter(dd);
  LOG(LL_VERBOSE_DEBUG, ("%p write %u @ 0x%x -> %d", dev, (unsigned int) len,
                         (unsigned int) offset, ret));
  return ret;
}

static bool mgos_vfs_dev_spi_flash_erase(struct mgos_vfs_dev *dev,
                                         size_t offset, size_t len) {
  bool ret = true;
  struct dev_data *dd = (struct dev_data *) dev->dev_data;
  if (offset % SPI_FLASH_SECTOR_SIZE != 0 || len % SPI_FLASH_SECTOR_SIZE != 0 ||
      offset >= dd->size) {
    ret = false;
  }
  ret = ret && spi_flash_dpd_exit(dd);
  ret = ret && spi_flash_wait_idle(dd);
  while (ret && len > 0) {
    uint32_t tx_data = htonl((dd->erase_sector_op << 24) | offset);
    ret = ret && spi_flash_wren(dd);
    ret = ret && spi_flash_op(dd, 4, &tx_data, 0, 0, NULL);
    ret = ret && spi_flash_wait_idle(dd);
    len -= SPI_FLASH_SECTOR_SIZE;
    offset += SPI_FLASH_SECTOR_SIZE;
  }
  spi_flash_dpd_enter(dd);
  LOG(LL_VERBOSE_DEBUG, ("%p erase %u @ 0x%x -> %d", dev, (unsigned int) len,
                         (unsigned int) offset, ret));
  return ret;
}

static size_t mgos_vfs_dev_spi_flash_get_size(struct mgos_vfs_dev *dev) {
  struct dev_data *dd = (struct dev_data *) dev->dev_data;
  return dd->size;
}

static bool mgos_vfs_dev_spi_flash_close(struct mgos_vfs_dev *dev) {
  /* Nothing to do. */
  (void) dev;
  return true;
}

static const struct mgos_vfs_dev_ops mgos_vfs_dev_spi_flash_ops = {
    .open = mgos_vfs_dev_spi_flash_open,
    .read = mgos_vfs_dev_spi_flash_read,
    .write = mgos_vfs_dev_spi_flash_write,
    .erase = mgos_vfs_dev_spi_flash_erase,
    .get_size = mgos_vfs_dev_spi_flash_get_size,
    .close = mgos_vfs_dev_spi_flash_close,
};

bool mgos_vfs_dev_spi_flash_init(void) {
  return mgos_vfs_dev_register_type(MGOS_VFS_DEV_TYPE_SPI_FLASH,
                                    &mgos_vfs_dev_spi_flash_ops);
}
