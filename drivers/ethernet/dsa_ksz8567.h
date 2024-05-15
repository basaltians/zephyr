/*
 * Copyright (c) 2024, Basalte bv
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DSA_KSZ8567_H__
#define __DSA_KSZ8567_H__

/* SPI commands */
#define KSZ8567_SPI_CMD_WR                              (BIT(6))
#define KSZ8567_SPI_CMD_RD                              (BIT(6) | BIT(5))

/* PHY registers */
#define KSZ8567_BMCR                                    0x00
#define KSZ8567_BMSR                                    0x01
#define KSZ8567_PHYID1                                  0x02
#define KSZ8567_PHYID2                                  0x03
#define KSZ8567_ANAR                                    0x04
#define KSZ8567_ANLPAR                                  0x05
#define KSZ8567_LINKMD                                  0x1D
#define KSZ8567_PHYSCS                                  0x1F

/* SWITCH registers */
#define KSZ8567_CHIP_ID0                                0x00
#define KSZ8567_CHIP_ID1                                0x01
#define KSZ8567_CHIP_ID2                                0x02
#define KSZ8567_CHIP_ID3                                0x03
#define KSZ8567_PORT6_OP_CTRL0                          0x6020
#define KSZ8567_PORTn_OP_CTRL0_TAIL_TAG_EN              0x04
#define KSZ8567_PORT6_XMII_CTRL0                        0x6300
#define KSZ8567_PORT6_XMII_CTRL1                        0x6301
#define KSZ8567_SWITCH_MAC_ADDR0                        0x0302
#define KSZ8567_SWITCH_MAC_ADDR1                        0x0303
#define KSZ8567_SWITCH_MAC_ADDR2                        0x0304
#define KSZ8567_SWITCH_MAC_ADDR3                        0x0305
#define KSZ8567_SWITCH_MAC_ADDR4                        0x0306
#define KSZ8567_SWITCH_MAC_ADDR5                        0x0307
#define KSZ8567_SWITCH_MAC_CTRL0_FRAME_LEN_CHECK_EN     0x08
#define KSZ8567_SWITCH_MAC_CTRL0                        0x0330
#define KSZ8567_SWITCH_LUE_CTRL0                        0x0310
#define KSZ8567_SWITCH_LUE_CTRL0_AGE_COUNT_DEFAULT      0x20
#define KSZ8567_SWITCH_LUE_CTRL0_HASH_OPTION_CRC        0x01
#define KSZ8567_SWITCH_LUE_CTRL3                        0x0313
#define KSZ8567_SWITCH_LUE_CTRL3_AGE_PERIOD_DEFAULT     0x4B
#define KSZ8567_PORT6_XMII_CTRL1                        0x6301
#define KSZ8567_PORTn_XMII_CTRL1_RGMII_ID_IG            0x10
#define KSZ8567_PORTn_XMII_CTRL1_RGMII_ID_EG            0x08

#define KSZ8567_PORTn_MSTP_STATE(port)                  (0x1B04 + ((port) * 0x1000))
#define KSZ8567_PORTn_MSTP_TRANSMIT_EN                  BIT(2)
#define KSZ8567_PORTn_MSTP_RECEIVE_EN                   BIT(1)
#define KSZ8567_PORTn_MSTP_LEARNING_DIS                 BIT(0)

#define KSZ8567_STAT2_PORTn(n)                          (0x1103 + ((n) * 0x1000))
#define KSZ8567_STAT2_LINK_GOOD                         BIT(2)

#define KSZ8567_CHIP_ID0_ID_DEFAULT                     0x00
#define KSZ8567_CHIP_ID1_ID_DEFAULT                     0x85
#define KSZ8863_SOFTWARE_RESET_SET                      BIT(0)

enum {
	/* We are assuming that port 7 is not being used
	 * KSZ8567 register's MAP
	 * (0x00 - 0x0F): Global Registers
	 * Port registers (offsets):
	 * (0x10): Port 1
	 * (0x20): Port 2
	 * (0x30): Port 3
	 * (0x40): Reserved
	 * (0x50): Port 4
	 */
	/* LAN ports for the ksz8794 switch */
	KSZ8567_PORT1 = 0,
	KSZ8567_PORT2,
	KSZ8567_PORT3,
	KSZ8567_PORT4,
	KSZ8567_PORT5,
	/*
	 * SWITCH <-> CPU port
	 *
	 * We also need to consider the "Reserved' offset
	 * defined above.
	 */
	KSZ8567_PORT6 = 5,
        KSZ8567_PORT7 = 6,
};


#define KSZ8567_STATIC_MAC_TABLE_VALID                  BIT(5)
#define KSZ8567_STATIC_MAC_TABLE_OVRD                   BIT(6)

#define KSZ8XXX_CHIP_ID0                                KSZ8567_CHIP_ID0
#define KSZ8XXX_CHIP_ID1                                KSZ8567_CHIP_ID1
#define KSZ8XXX_CHIP_ID0_ID_DEFAULT                     KSZ8567_CHIP_ID0_ID_DEFAULT
#define KSZ8XXX_CHIP_ID1_ID_DEFAULT                     KSZ8567_CHIP_ID1_ID_DEFAULT
#define KSZ8XXX_FIRST_PORT                              KSZ8567_PORT1
#define KSZ8XXX_LAST_PORT                               KSZ8567_PORT5
#define KSZ8XXX_CPU_PORT                                KSZ8567_PORT6
#define KSZ8XXX_REG_IND_CTRL_0                          KSZ8567_REG_IND_CTRL_0
#define KSZ8XXX_REG_IND_CTRL_1                          KSZ8567_REG_IND_CTRL_1
#define KSZ8XXX_REG_IND_DATA_8                          KSZ8567_REG_IND_DATA_8
#define KSZ8XXX_REG_IND_DATA_7                          KSZ8567_REG_IND_DATA_7
#define KSZ8XXX_REG_IND_DATA_6                          KSZ8567_REG_IND_DATA_6
#define KSZ8XXX_REG_IND_DATA_5                          KSZ8567_REG_IND_DATA_5
#define KSZ8XXX_REG_IND_DATA_4                          KSZ8567_REG_IND_DATA_4
#define KSZ8XXX_REG_IND_DATA_3                          KSZ8567_REG_IND_DATA_3
#define KSZ8XXX_REG_IND_DATA_2                          KSZ8567_REG_IND_DATA_2
#define KSZ8XXX_REG_IND_DATA_1                          KSZ8567_REG_IND_DATA_1
#define KSZ8XXX_REG_IND_DATA_0                          KSZ8567_REG_IND_DATA_0
#define KSZ8XXX_STATIC_MAC_TABLE_VALID                  KSZ8567_STATIC_MAC_TABLE_VALID
#define KSZ8XXX_STATIC_MAC_TABLE_OVRD                   KSZ8567_STATIC_MAC_TABLE_OVRD
#define KSZ8XXX_STAT2_LINK_GOOD                         KSZ8567_STAT2_LINK_GOOD
#define KSZ8XXX_RESET_REG                               KSZ8567_CHIP_ID3
#define KSZ8XXX_RESET_SET                               KSZ8863_SOFTWARE_RESET_SET
#define KSZ8XXX_RESET_CLEAR                             0
#define KSZ8XXX_STAT2_PORTn                             KSZ8567_STAT2_PORTn
#define KSZ8XXX_SPI_CMD_RD                              KSZ8567_SPI_CMD_RD
#define KSZ8XXX_SPI_CMD_WR                              KSZ8567_SPI_CMD_WR
#define KSZ8XXX_SOFT_RESET_DURATION                     1000
#define KSZ8XXX_HARD_RESET_WAIT                         100000
#endif /* __DSA_KSZ8567_H__ */
