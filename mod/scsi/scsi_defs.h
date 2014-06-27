/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#ifndef SCSI_DEFS_H
#define SCSI_DEFS_H

#include "types.h"

//ringbuffer. must be pow 2
#define SCSI_ERROR_BUF_SIZE							8
#define SCSI_ERROR_BUF_MASK							(SCSI_ERROR_BUF_SIZE - 1)

//described in RBC
#define SCSI_CMD_TEST_UNIT_READY						0x00
#define SCSI_CMD_REQUEST_SENSE						0x03
#define SCSI_CMD_READ6									0x08
#define SCSI_CMD_WRITE6									0x0A

#define SCSI_CMD_INQUIRY								0x12
#define SCSI_CMD_VERIFY6								0x13
#define SCSI_CMD_MODE_SELECT6							0x15
#define SCSI_CMD_MODE_SENSE6							0x1A
#define SCSI_CMD_START_STOP_UNIT						0x1b
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL	0x1e

#define SCSI_CMD_READ_FORMAT_CAPACITY				0x23
#define SCSI_CMD_READ_CAPACITY						0x25
#define SCSI_CMD_READ10									0x28
#define SCSI_CMD_WRITE10								0x2a
#define SCSI_CMD_VERIFY10								0x2f

#define SCSI_CMD_SYNCHRONIZE_CACHE					0x35
#define SCSI_CMD_WRITE_BUFFER							0x3B

#define SCSI_CMD_MODE_SELECT10						0x55
#define SCSI_CMD_MODE_SENSE10							0x5A

#define SCSI_CMD_READ16									0x88
#define SCSI_CMD_WRITE16								0x8A
#define SCSI_CMD_VERIFY16								0x8F

#define SCSI_CMD_READ12									0xA8
#define SCSI_CMD_WRITE12								0xAA
#define SCSI_CMD_VERIFY12								0xAF

#define SCSI_INQUIRY_CMD_DT							(1 << 1)
#define SCSI_INQUIRY_EVPD								(1 << 0)

#define SCSI_VERIFY_BYTCHK								(1 << 1)

//codes for EPVD
#define INQUIRY_VITAL_PAGE_SUPPORTED_PAGES		0x00
#define INQUIRY_VITAL_PAGE_SERIAL_NUM				0x80
#define INQUIRY_VITAL_PAGE_ASCII_OPERATIONS		0x82
#define INQUIRY_VITAL_PAGE_DEVICE_INFO				0x83

//sense key for error recovery
#define SENSE_KEY_NO_SENSE								0x00
#define SENSE_RECOVERED_ERROR							0x01
#define SENSE_KEY_NOT_READY							0x02
#define SENSE_KEY_MEDIUM_ERROR						0x03
#define SENSE_KEY_HARDWARE_ERROR						0x04
#define SENSE_KEY_ILLEGAL_REQUEST					0x05
#define SENSE_KEY_UNIT_ATTENTION						0x06
#define SENSE_KEY_DATA_PROTECT						0x07
#define SENSE_KEY_BLANK_CHECK							0x08
#define SENSE_KEY_VENDOR_SPECIFIC					0x09
#define SENSE_KEY_COPY_ABORTED						0x0a
#define SENSE_KEY_ABORTED_COMMAND					0x0b
#define SENSE_KEY_VOLUME_OVERFLOW					0x0d
#define SENSE_KEY_MISCOMPARE							0x0e

//ASC + ASQ qualifier codes
#define ASQ_NO_ADDITIONAL_SENSE_INFORMATION		0x0000
#define ASQ_PERIPHERAL_DEVICE_WRITE_FAULT			0x0300
#define ASQ_LOGICAL_UNIT_COMMUNICATION_FAILURE	0x0800
#define ASQ_LOGICAL_UNIT_COMMUNICATION_TIMEOUT	0x0801
#define ASQ_WRITE_ERROR									0x0c00
#define ASQ_ERROR_LOG_OVERFLOW						0x0a00
#define ASQ_UNRECOVERED_READ_ERROR					0x1100
#define ASQ_READ_RETRIES_EXHAUSTED					0x1101
#define ASQ_MISCOMPARE_DURING_VERIFY_OPERATION	0x1d00
#define ASQ_INVALID_COMMAND_OPERATION_CODE		0x2000
#define ASQ_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE	0x2101

#define ASQ_INVALID_FIELD_IN_CDB						0x2400
#define ASQ_CDB_DECRYPTION_ERROR						0x2401

#define ASQ_INVALID_FIELD_IN_PARAMETER_LIST		0x2600
#define ASQ_WRITE_PROTECTED							0x2700
#define ASQ_COMMAND_DEVICE_INTERNAL_RESET			0x2904
#define ASQ_COMMAND_SEQUENCE_ERROR					0x2c00
#define ASQ_MEDIUM_NOT_PRESENT						0x3a00
#define ASQ_COMMAND_PHASE_ERROR						0x4A00
#define ASQ_DATA_PHASE_ERROR							0x4B00

typedef enum {
	SCSI_STATE_IDLE,
	SCSI_STATE_READ,
	SCSI_STATE_WRITE,
	SCSI_STATE_VERIFY,
	SCSI_STATE_SEND_STATUS
} SCSI_STATE;

typedef enum {
	SCSI_CMD_6 = 6,
	SCSI_CMD_10 = 10,
	SCSI_CMD_12 = 12,
	SCSI_CMD_16 = 16
} SCSI_CMD_TYPE;

typedef struct {
	uint8_t	code;
	uint16_t	asq;
} SCSI_ERROR;

typedef struct {
	SCSI_CMD_TYPE cmd_type;
	uint8_t opcode;
	uint8_t flags;
	uint32_t address, additional_data, len;
	uint8_t misc;
	uint8_t control;
} SCSI_CMD;

#pragma pack(push, 1)

typedef struct {

} SCSI_INQUIRY_TYPE;

#pragma pack(pop)

#endif // SCSI_DEFS_H
