/*
 * Nordic Legacy DFU Tool written in C
 * based on: 
 * https://github.com/infsoft-locaware/nrfdfu
 * https://github.com/recrof/nrf_dfu_py
 * https://github.com/michaelrsweet/zipc
 * https://github.com/rpz80/json
 *
 *
 * Copyright © 2026 by Michal Moranski.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include "log.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/l2cap.h>
#include <errno.h>
#include <getopt.h>
#include "json.h"
#include "zipc.h"
#include "conf.h"

#define DFU_CONTROL_POINT_HANDLE 0x0012 
#define DFU_PACKET_HANDLE        0x0010 


#define OP_CODE_START_DFU                0x01
#define OP_CODE_INIT_DFU_PARAMS          0x02
#define OP_CODE_RECEIVE_FIRMWARE_IMAGE   0x03
#define OP_CODE_VALIDATE                 0x04
#define OP_CODE_ACTIVATE_AND_RESET       0x05
#define OP_CODE_RESET			 0x06
#define OP_CODE_PACKET_RECEIPT_NOTIF_REQ 0x08
#define OP_CODE_RESPONSE_CODE            0x10
#define OP_CODE_PACKET_RECEIPT_NOTIF     0x11

#define UPLOAD_MODE_APPLICATION          0x04

// ATT Opcodes
#define ATT_OP_WRITE_REQ                 0x12
#define ATT_OP_WRITE_CMD                 0x52 // Write without response
#define ATT_OP_HANDLE_VAL_NOTIF          0x1b


// Standard BlueZ kernel constants for control channel
//#define AF_BLUETOOTH        31
#define BTPROTO_HCI         1
#define HCI_CHANNEL_CONTROL 3

// Mgmt API Command Opcodes
#define MGMT_OP_SET_POWERED 0x0005
#define MGMT_OP_SET_LE      0x000D


struct config conf;


// Strict BlueZ kernel Mgmt header layout
struct mgmt_hdr {
    uint16_t opcode;
    uint16_t index;  // Controller ID (e.g. 0 for hci0)
    uint16_t len;    // Length of the payload following this header
} __attribute__((packed));

// Explicit definition container for a single uint8 parameter command
struct mgmt_mode_cmd {
    struct mgmt_hdr hdr;
    uint8_t val;
} __attribute__((packed));

static int send_mgmt_cmd(int sk, uint16_t opcode, uint16_t index, uint8_t value) {
    struct mgmt_mode_cmd cmd;
    
    cmd.hdr.opcode = htole16(opcode);
    cmd.hdr.index  = htole16(index);
    cmd.hdr.len    = htole16(sizeof(uint8_t)); // Crucial: must equal trailing data size
    cmd.val        = value;

    if (write(sk, &cmd, sizeof(cmd)) < 0) {
        perror("Mgmt write failed");
        return -1;
    }
    return 0;
}


int configure_hci0_for_ble(void) {
    int sk;
    struct sockaddr_hci addr;
    uint16_t controller_idx = 0; // hci0

    // 1. Open the BlueZ Kernel Management Control Channel
    sk = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (sk < 0) {
        perror("Failed to open mgmt socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_channel = HCI_CHANNEL_CONTROL; // Talk directly to kernel mgmt
    addr.hci_dev = HCI_DEV_NONE; // Non-specific adapter for binding

    if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("Failed to bind mgmt socket");
        close(sk);
        return -1;
    }

    // 1. Force absolute power OFF first.
    // Older kernels reject LE toggles if the driver thinks it's still alive.
    if (send_mgmt_cmd(sk, MGMT_OP_SET_POWERED, controller_idx, 0) < 0) {
        fprintf(stderr, "Failed to power off controller\n");
    }
    usleep(300000); // 300ms: Embedded controllers need longer to reset state machines

    // 2. Force Low Energy (LE) Mode ON
    if (send_mgmt_cmd(sk, MGMT_OP_SET_LE, controller_idx, 1) < 0) {
        fprintf(stderr, "Failed to set LE command\n");
        close(sk);
        return -1;
    }
    usleep(200000);

    // 3. Turn Power back ON 
    if (send_mgmt_cmd(sk, MGMT_OP_SET_POWERED, controller_idx, 1) < 0) {
        fprintf(stderr, "Failed to power on controller\n");
        close(sk);
        return -1;
    }
    usleep(300000); // Allow hardware link layer to settle



    close(sk);
    return 0;
}






static bool read_manifest(zipc_t* zip, char* ap_dat, char* ap_bin)
{
    bool ret = false;
    char buf[600];
    const int kErrorBufSize = 1024;
    struct JsonVal val;
    struct JsonVal *inner, *app, *dat;
    char errBuf[kErrorBufSize];

    zipc_file_t* zf = zipcOpenFile(zip, "manifest.json");
    if (zf == NULL) {
	LOG_ERR("ZIP file does not contain manifest");
	return false;
    }

    ssize_t len = zipcFileRead(zf, buf, sizeof(buf));
    if (len <= 0) {
	LOG_ERR("Could not read Manifest");
	goto exit;
    }


    /* read JSON */

    val = jsonParseString(buf, errBuf, kErrorBufSize);

    if (*errBuf != 0) // Parsing failed
    {
	LOG_ERR("Error parsing manifest.json: %s\n", errBuf);
	goto exit;
    }


    inner = JsonVal_getObjectValueByKey(&val, "manifest");
    if(JsonVal_isObject(inner) != 1) {
    	LOG_ERR("manifest.json: no \"manifest\" entry.");
	goto exit;
    }

    inner = JsonVal_getObjectValueByKey(inner, "application");

    if(JsonVal_isObject(inner) != 1) {
    	LOG_ERR("manifest.json: no \"application\" entry.");
	JsonVal_destroy(&val);
	goto exit;
    }


    app = JsonVal_getObjectValueByKey(inner, "bin_file");
    if(JsonVal_isString(app) != 1) {
    	LOG_ERR("manifest.json: no \"bin_file\" entry.");
	goto exit;
    } else strcpy(ap_bin,app->u.string);


    dat = JsonVal_getObjectValueByKey(inner, "dat_file");
    if(JsonVal_isString(dat) != 1) {
    	LOG_ERR("manifest.json: no \"dat_file\" entry.");
	goto exit;
    } else; strcpy(ap_dat,dat->u.string);



    JsonVal_destroy(&val);
    ret = true;

exit:

    zipcFileFinish(zf);
    return ret;
}



void increment_bdaddr(bdaddr_t *ba) {
    for (int i = 0; i < 6; i++) {
        ba->b[i] += 1;
        if (ba->b[i] != 0x00) {
            break; // No carry needed, stop incrementing
        }
        // Carry over to the next byte
    }
}

int wait_for_dfu_response(int sock, uint8_t expected_dfu_op, int timeout_sec) {
    uint8_t rx_buf[64];
    fd_set fds;
    struct timeval tv = {timeout_sec, 0};

    while (1) {
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        
        if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0) return -1;
        int len = read(sock, rx_buf, sizeof(rx_buf));
	/*
	printf("RX:");
	for (int i=0; i<len; i++) printf("%02X ",rx_buf[i]);
	printf("\n");
        */
	if (len > 0 && (rx_buf[0] == ATT_OP_HANDLE_VAL_NOTIF || rx_buf[0] == 0x1d)) {
            uint16_t handle = rx_buf[1] | (rx_buf[2] << 8);
            if (handle == DFU_CONTROL_POINT_HANDLE && rx_buf[3] == OP_CODE_RESPONSE_CODE) {
                if (rx_buf[4] == expected_dfu_op && rx_buf[5] == 1) return 0;
            }
            if (rx_buf[3] == OP_CODE_PACKET_RECEIPT_NOTIF) return 0;
        }
    }
    return -1;
}

int gatt_write(int sock, uint16_t handle, uint8_t *data, size_t data_len, int response) {
    uint8_t tx_buf[256];
    tx_buf[0] = response ? ATT_OP_WRITE_REQ : ATT_OP_WRITE_CMD;
    tx_buf[1] = handle & 0xFF;
    tx_buf[2] = (handle >> 8) & 0xFF;
    memcpy(&tx_buf[3], data, data_len);
    
    if (write(sock, tx_buf, 3 + data_len) < 0) return -1;
    if (response) {
        uint8_t rx_buf[16];
        return (read(sock, rx_buf, sizeof(rx_buf)) > 0 && rx_buf[0] == 0x13) ? 0 : -1;
    }
    return 0;
}



int gatt_write2(int sock, int s2, uint16_t handle, uint8_t *data, size_t data_len, int response) {
    uint8_t tx_buf[256];
    tx_buf[0] = response ? ATT_OP_WRITE_REQ : ATT_OP_WRITE_CMD;
    tx_buf[1] = handle & 0xFF;
    tx_buf[2] = (handle >> 8) & 0xFF;
    memcpy(&tx_buf[3], data, data_len);
    
    if (write(sock, tx_buf, 3 + data_len) < 0) return -1;
    if (response) {
        uint8_t rx_buf[16];
        return (read(s2, rx_buf, sizeof(rx_buf)) > 0 && rx_buf[0] == 0x13) ? 0 : -1;
    }
    return 0;
}


static struct option ble_options[] = {{"help", no_argument, NULL, 'h'},
					{"verbose", optional_argument, NULL, 'v'},
					{"addr", required_argument, NULL, 'a'},
					{"atype", optional_argument, NULL, 't'},
					{"intf", optional_argument, NULL, 'i'},
					{"noincr", optional_argument, NULL, 'n'},
				      {NULL, 0, NULL, 0}};


static void usage(void)
{
    fprintf(stderr,
	    "Usage: nrfdfu [options] DFUPKG.zip\n"
	    "Nordic NRF DFU Upgrade with DFUPKG.zip\n"
	    "Options :\n"
	    "  -h, --help\t\tShow help\n"
	    "  -v, --verbose=<level>\tLog level 1 or 2 (-vv)\n"
	    "  -a, --addr <mac>\tBLE MAC address to connect to\n"
	    "  -t, --atype public|random\tBLE MAC address type (optional)\n"
	    "  -i, --intf <name>\tBT interface name (hci0)\n"
	    "  -n, --noincr\tDo not increment MAC after jumping to bootloader\n"

    );
}




static void main_options(int argc, char* argv[])
{
    /* defaults */

    conf.loglevel = LL_NOTICE;
    conf.timeout = 10;
    conf.ble_atype = BAT_UNKNOWN;
    conf.interface = "hci0";
    conf.noincr = false;

    if (argc <= 1) {
	usage();
	exit(EXIT_FAILURE);
    }
    const char* type = argv[1];

    int n = 0;
    while (n >= 0) {

	n = getopt_long(argc, argv, "hv::a:t:i:n", ble_options, NULL);

	if (n < 0)
	    continue;
	switch (n) {
	case '?':
	    exit(EXIT_FAILURE);
	case 'h':
	    usage();
	    exit(EXIT_SUCCESS);
	case 'v':
	    if (optarg == NULL)
		conf.loglevel = LL_INFO;
	    else if (optarg[0] == 'v' || optarg[0] == '2')
		conf.loglevel = LL_DEBUG;
	    break;
	case 't':
		if (strncasecmp(optarg, "pub", 3) == 0) {
		    conf.ble_atype = BDADDR_LE_PUBLIC;
		} else if (strncasecmp(optarg, "rand", 4) == 0) {
		    conf.ble_atype = BDADDR_LE_RANDOM;
		}

	    break;
	case 'a':
	    conf.ble_addr = optarg;
	    break;
	case 'i':
	    conf.interface = optarg;
	    break;
	case 'n':
	    conf.noincr = true;
	    break;

	}
    }

    if (argc > 2 && optind < argc && strcmp(argv[argc - 1], type) != 0) {
	conf.zipfile = argv[argc - 1];
    } else {
	LOG_ERR("ZIP file missing");
	exit(EXIT_FAILURE);
    }
}
void enter_bootloader(void) {

    int client;
    int sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    struct sockaddr_l2 src = {0}, dst = {0};
    socklen_t opt = sizeof(dst);

    src.l2_family = AF_BLUETOOTH;
    src.l2_cid = htobs(4); // ATT CID

    if (bind(sock, (struct sockaddr *)&src, sizeof(src)) < 0) {
        LOG_ERR("Failed to bind socket: %s", strerror(errno));
    }
    dst.l2_family = AF_BLUETOOTH;
    str2ba(conf.ble_addr, &dst.l2_bdaddr);
    dst.l2_cid = htobs(4);
    dst.l2_bdaddr_type = conf.ble_atype;

    struct l2cap_options opts;
    socklen_t optlen = sizeof(opts);
    getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen);
    opts.mode = 0; // L2CAP_MODE_BASIC
    LOG_ERR("L2CAP_options: omtu: %d imtu: %d flush_to %d mode: %d fcs: %d max_tx: %d txwin_size %d", \
    opts.omtu,opts.imtu,opts.flush_to,opts.mode,opts.fcs,opts.max_tx,opts.txwin_size);
    if (setsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts)) < 0) {
	LOG_ERR("Failed to set L2CAP options");
    }

    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
	LOG_ERR("Could not connect: %s, trying bootloader mode...",strerror(errno));
	return;

    } else LOG_INF("Connected.");

    //client = accept(sock,(struct sockaddr *)&dst, &opt);

    uint8_t cccd_data[2] = {0x01, 0x00};
    gatt_write(sock, DFU_CONTROL_POINT_HANDLE + 1, cccd_data, 2, 1); // CCCD
    uint8_t cccd_data1[2] = {0x01, 0x04};
    gatt_write(sock, DFU_CONTROL_POINT_HANDLE, cccd_data1, 2, 1); // CCCD
    LOG_INF("Waiting for bootloader mode");

    close(sock);
    sleep(1);


}


void dfu_upgrade(zipc_t* zip, const char* dat_name, const char* fw_name) {

#define TRIES 3

    uint32_t sd=0, bl=0, app_size=0;
    ssize_t bytes_read;
    zipc_file_t *fw_zip;
    int tries;
    uint8_t buf2[512];
    ssize_t init_len,fw_len;
    int sock;
    struct sockaddr_l2 src = {0}, dst = {0};

    configure_hci0_for_ble();
    enter_bootloader();

    for (tries=0; tries<TRIES; tries++) {

        sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sock < 0) {
    	    LOG_ERR("Failed to create bootloader socket");
    	    return;
	}
	memset(&src, 0, sizeof(src));
	src.l2_family = AF_BLUETOOTH;
	src.l2_cid = htobs(4);
	if (bind(sock, (struct sockaddr *)&src, sizeof(src)) < 0) {
    	    LOG_ERR("Failed to bind bootloader socket");
    	    close(sock);
    	    return;
	}
	str2ba(conf.ble_addr, &dst.l2_bdaddr);
	if (!conf.noincr) increment_bdaddr(&dst.l2_bdaddr);
	dst.l2_family = AF_BLUETOOTH;
	dst.l2_cid = htobs(4);
	dst.l2_bdaddr_type = conf.ble_atype;

	if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
	    LOG_ERR("Could not connect to bootloader!");
	    close(sock);
	    return;
	} else LOG_INF("Connected (bootloader mode)");

	zipc_file_t *zf = zipcOpenFile(zip, dat_name);
	init_len = zipcFileRead(zf, buf2, sizeof(buf2));

	if (init_len < 0) {
	    LOG_ERR("zip_fread error");
	    return;
	}
	zipcFileFinish(zf);

	fw_zip = zipcOpenFile(zip, fw_name);
	fw_len = zipcFileGetUncompressedSize(fw_zip);
	app_size=fw_len;
	uint8_t cccd_data3[2] = {0x01, 0x00};
	gatt_write(sock, DFU_CONTROL_POINT_HANDLE + 1, cccd_data3, 2, 1); // CCCD
	uint8_t start_p[2] = {OP_CODE_START_DFU, UPLOAD_MODE_APPLICATION};
	gatt_write(sock, DFU_CONTROL_POINT_HANDLE, start_p, 2, 1);
	uint8_t size_p[12];
	memcpy(size_p, &sd, 4); memcpy(size_p+4, &bl, 4); memcpy(size_p+8, &app_size, 4);
	gatt_write(sock, DFU_PACKET_HANDLE, size_p, 12, 0);
	if (wait_for_dfu_response(sock, OP_CODE_START_DFU, 10) == 0) LOG_INF("Sizes sent.");
	else {
	    LOG_ERR("Error sending sizes. Sending RESET...");
	    uint8_t reset_cmd[1] = {OP_CODE_RESET};
	    gatt_write(sock, DFU_CONTROL_POINT_HANDLE, reset_cmd, 1, 1);
	    sleep(1);
	    continue;
	}
	// 2. Init Packet
	uint8_t init_s[2] = {OP_CODE_INIT_DFU_PARAMS, 0x00};
	gatt_write(sock, DFU_CONTROL_POINT_HANDLE, init_s, 2, 1);
	gatt_write(sock, DFU_PACKET_HANDLE, buf2, init_len, 0);
	uint8_t init_e[2] = {OP_CODE_INIT_DFU_PARAMS, 0x01};
	gatt_write(sock, DFU_CONTROL_POINT_HANDLE, init_e, 2, 1);
	if (wait_for_dfu_response(sock, OP_CODE_INIT_DFU_PARAMS, 10)==0) LOG_INF("DAT init sent.");
	else {
	    LOG_ERR("Error sending DAT");
	    continue;
	}
	// 3. PRN & Firmware
	uint8_t prn_p[3] = {OP_CODE_PACKET_RECEIPT_NOTIF_REQ, 10, 0}; // PRN=10
	gatt_write(sock, DFU_CONTROL_POINT_HANDLE, prn_p, 3, 1);
	uint8_t rec_cmd[1] = {OP_CODE_RECEIVE_FIRMWARE_IMAGE};
	gatt_write(sock, DFU_CONTROL_POINT_HANDLE, rec_cmd, 1, 1);
	break;
    }
    if (tries==TRIES) {
	LOG_ERR("Too many failed attempts. Exiting.");
	return;
    }

    for(int i=0; i<app_size; i+=20) {
        uint8_t chunk[20];
	bytes_read = zipcFileRead(fw_zip, chunk, 20);
	gatt_write(sock, DFU_PACKET_HANDLE, chunk, bytes_read, 0);
        if ((i/20 + 1) % 10 == 0) {
	    if (wait_for_dfu_response(sock, OP_CODE_PACKET_RECEIPT_NOTIF, 5)==0) {
		printf("%d from %d bytes sent (%.f%%)\r",i, app_size, (float)i/app_size*100.0);
		fflush(stdout);
	    }
	    else LOG_ERR("Bytes sending error!");

	}

    }
    if (wait_for_dfu_response(sock, OP_CODE_RECEIVE_FIRMWARE_IMAGE, 20)==0) LOG_INF("\nFirmware sent.");
    else LOG_ERR("\nFirmware sending error!"); 

    // 4. Walidacja i Reset
    uint8_t val_cmd[1] = {OP_CODE_VALIDATE};
    gatt_write(sock, DFU_CONTROL_POINT_HANDLE, val_cmd, 1, 1);
    if (wait_for_dfu_response(sock, OP_CODE_VALIDATE, 10)==0) LOG_INF("Validate ok.");
    else LOG_ERR("Validation error!");

    uint8_t res_cmd[1] = {OP_CODE_ACTIVATE_AND_RESET};
    gatt_write(sock, DFU_CONTROL_POINT_HANDLE, res_cmd, 1, 1);

    if (fw_zip) zipcFileFinish(fw_zip);
    sleep(1);
    close(sock);

}


int main(int argc, char **argv) {

    char ap_dat[256];
    char ap_bin[256];

    main_options(argc, argv);

    zipc_t* zip = zipcOpen(conf.zipfile, "r");
    if (zip == NULL) {
	goto exit;
    }

    if (!read_manifest(zip, ap_dat, ap_bin)) {
	LOG_ERR("Could not read manifest file");
	goto exit;

    }

    if (strlen(ap_dat) && strlen(ap_bin)) {
	LOG_INF("Update contain files: %s, %s",ap_dat,ap_bin);
	dfu_upgrade(zip, ap_dat, ap_bin);

    }

exit:

    if (zip) {
	zipcClose(zip);
    }

    return 0;
}
