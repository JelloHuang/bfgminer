/**
 *   ztex.c - BFGMiner worker for Ztex 1.15x/y fpga board
 *
 *   Copyright (c) 2012 nelisky.btc@gmail.com
 *
 *   This work is based upon the Java SDK provided by ztex which is
 *   Copyright (C) 2009-2011 ZTEX GmbH.
 *   http://www.ztex.de
 *
 *   This work is based upon the icarus.c worker which is
 *   Copyright 2012 Luke Dashjr
 *   Copyright 2012 Xiangfu <xiangfu@openmobilefree.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2 as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see http://www.gnu.org/licenses/.
**/

#include <unistd.h>
#include <sha2.h>

#include "dynclock.h"
#include "fpgautils.h"
#include "miner.h"
#include "libztex.h"

#define GOLDEN_BACKLOG 5

struct device_api ztex_api;

// Forward declarations
static void ztex_disable(struct thr_info* thr);
static bool ztex_prepare(struct thr_info *thr);

static void ztex_selectFpga(struct libztex_device* ztex)
{
	if (ztex->root->numberOfFpgas > 1) {
		if (ztex->root->selectedFpga != ztex->fpgaNum)
			mutex_lock(&ztex->root->mutex);
		libztex_selectFpga(ztex);
	}
}

static void ztex_releaseFpga(struct libztex_device* ztex)
{
	if (ztex->root->numberOfFpgas > 1) {
		ztex->root->selectedFpga = -1;
		mutex_unlock(&ztex->root->mutex);
	}
}

static struct cgpu_info *ztex_setup(struct libztex_device *dev, int j)
{
	struct cgpu_info *ztex;
	char fpganame[LIBZTEX_SNSTRING_LEN+3+1];

	ztex = calloc(1, sizeof(struct cgpu_info));
	ztex->api = &ztex_api;
	ztex->device_ztex = dev;
	ztex->threads = 1;
	dev->fpgaNum = j;
	add_cgpu(ztex);
	sprintf(ztex->device_ztex->repr, "%s %u", ztex->api->name, ztex->device_id);
	sprintf(fpganame, "%s-%u", ztex->device_ztex->snString, j+1);
	ztex->name = strdup(fpganame);
	applog(LOG_INFO, "%s %u: Found Ztex (ZTEX %s)", ztex->api->name, ztex->device_id, fpganame);

	return ztex;
}

static int ztex_autodetect(void)
{
	int cnt;
	int i,j;
	int fpgacount;
	int totaldevs = 0;
	struct libztex_dev_list **ztex_devices;
	struct libztex_device *ztex_master;
	struct libztex_device *ztex_slave;
	struct cgpu_info *ztex;

	cnt = libztex_scanDevices(&ztex_devices);
	if (cnt > 0)
		applog(LOG_INFO, "Found %d ztex board%s", cnt, cnt > 1 ? "s" : "");

	for (i = 0; i < cnt; i++) {
		ztex_master = ztex_devices[i]->dev;
		ztex_master->root = ztex_master;
		ztex = ztex_setup(ztex_master, 0);

		fpgacount = libztex_numberOfFpgas(ztex->device_ztex);
		totaldevs += fpgacount;

		if (fpgacount > 1)
			pthread_mutex_init(&ztex->device_ztex->mutex, NULL);

		for (j = 1; j < fpgacount; j++) {
			ztex_slave = calloc(1, sizeof(struct libztex_device));
			memcpy(ztex_slave, ztex_master, sizeof(struct libztex_device));
			ztex_slave->root = ztex_master;
			ztex_setup(ztex_slave, j);
		}
	}

	if (cnt > 0)
		libztex_freeDevList(ztex_devices);

	return totaldevs;
}

static void ztex_detect()
{
	// This wrapper ensures users can specify -S ztex:noauto to disable it
	noserial_detect(&ztex_api, ztex_autodetect);
}

static bool ztex_change_clock_func(struct thr_info *thr, int bestM)
{
	struct libztex_device *ztex = thr->cgpu->device_ztex;

	ztex_selectFpga(ztex);
	libztex_setFreq(ztex, bestM);
	ztex_releaseFpga(ztex);

	return true;
}

static bool ztex_updateFreq(struct thr_info *thr)
{
	struct libztex_device *ztex = thr->cgpu->device_ztex;
	bool rv = dclk_updateFreq(&ztex->dclk, ztex_change_clock_func, thr);
	if (unlikely(!rv)) {
		ztex_selectFpga(ztex);
		libztex_resetFpga(ztex);
		ztex_releaseFpga(ztex);
	}
	return rv;
}

static bool ztex_checkNonce(struct libztex_device *ztex,
                            struct work *work,
                            struct libztex_hash_data *hdata)
{
	uint32_t *data32 = (uint32_t *)(work->data);
	unsigned char swap[128];
	uint32_t *swap32 = (uint32_t *)swap;
	unsigned char hash1[32];
	unsigned char hash2[32];
	uint32_t *hash2_32 = (uint32_t *)hash2;

	hdata->nonce = le32toh(hdata->nonce);
	hdata->hash7 = le32toh(hdata->hash7);

	work->data[64 + 12 + 0] = (hdata->nonce >> 0) & 0xff;
	work->data[64 + 12 + 1] = (hdata->nonce >> 8) & 0xff;
	work->data[64 + 12 + 2] = (hdata->nonce >> 16) & 0xff;
	work->data[64 + 12 + 3] = (hdata->nonce >> 24) & 0xff;

	swap32yes(swap32, data32, 80 / 4);
	
	sha2(swap, 80, hash1, false);
	sha2(hash1, 32, hash2, false);
	if (htobe32(hash2_32[7]) != ((hdata->hash7 + 0x5be0cd19) & 0xFFFFFFFF)) {
		dclk_errorCount(&ztex->dclk, 1.0 / ztex->numNonces);
		applog(LOG_DEBUG, "%s: checkNonce failed for %0.8X", ztex->repr, hdata->nonce);
		return false;
	}
	return true;
}

static int64_t ztex_scanhash(struct thr_info *thr, struct work *work,
                              __maybe_unused int64_t max_nonce)
{
	struct libztex_device *ztex;
	unsigned char sendbuf[44];
	int i, j, k;
	uint32_t *backlog;
	int backlog_p = 0, backlog_max;
	uint32_t *lastnonce;
	uint32_t nonce, noncecnt = 0;
	bool overflow, found;
	struct libztex_hash_data hdata[GOLDEN_BACKLOG];

	ztex = thr->cgpu->device_ztex;

	memcpy(sendbuf, work->data + 64, 12);
	memcpy(sendbuf + 12, work->midstate, 32);
	
	ztex_selectFpga(ztex);
	i = libztex_sendHashData(ztex, sendbuf);
	if (i < 0) {
		// Something wrong happened in send
		applog(LOG_ERR, "%s: Failed to send hash data with err %d, retrying", ztex->repr, i);
		nmsleep(500);
		i = libztex_sendHashData(ztex, sendbuf);
		if (i < 0) {
			// And there's nothing we can do about it
			ztex_disable(thr);
			applog(LOG_ERR, "%s: Failed to send hash data with err %d, giving up", ztex->repr, i);
			ztex_releaseFpga(ztex);
			return -1;
		}
	}
	ztex_releaseFpga(ztex);
	
	applog(LOG_DEBUG, "%s: sent hashdata", ztex->repr);

	lastnonce = calloc(1, sizeof(uint32_t)*ztex->numNonces);
	if (lastnonce == NULL) {
		applog(LOG_ERR, "%s: failed to allocate lastnonce[%d]", ztex->repr, ztex->numNonces);
		return -1;
	}

	/* Add an extra slot for detecting dupes that lie around */
	backlog_max = ztex->numNonces * (2 + ztex->extraSolutions);
	backlog = calloc(1, sizeof(uint32_t) * backlog_max);
	if (backlog == NULL) {
		applog(LOG_ERR, "%s: failed to allocate backlog[%d]", ztex->repr, backlog_max);
		free(lastnonce);
		return -1;
	}
	
	overflow = false;

	applog(LOG_DEBUG, "%s: entering poll loop", ztex->repr);
	while (!(overflow || thr->work_restart)) {
		nmsleep(250);
		if (thr->work_restart) {
			applog(LOG_DEBUG, "%s: New work detected", ztex->repr);
			break;
		}
		ztex_selectFpga(ztex);
		i = libztex_readHashData(ztex, &hdata[0]);
		if (i < 0) {
			// Something wrong happened in read
			applog(LOG_ERR, "%s: Failed to read hash data with err %d, retrying", ztex->repr, i);
			nmsleep(500);
			i = libztex_readHashData(ztex, &hdata[0]);
			if (i < 0) {
				// And there's nothing we can do about it
				ztex_disable(thr);
				applog(LOG_ERR, "%s: Failed to read hash data with err %d, giving up", ztex->repr, i);
				free(lastnonce);
				free(backlog);
				ztex_releaseFpga(ztex);
				return -1;
			}
		}
		ztex_releaseFpga(ztex);

		if (thr->work_restart) {
			applog(LOG_DEBUG, "%s: New work detected", ztex->repr);
			break;
		}

		dclk_gotNonces(&ztex->dclk);
 
		for (i = 0; i < ztex->numNonces; i++) {
			nonce = hdata[i].nonce;
			nonce = le32toh(nonce);
			if (nonce > noncecnt)
				noncecnt = nonce;
			if (((nonce & 0x7fffffff) >> 4) < ((lastnonce[i] & 0x7fffffff) >> 4)) {
				applog(LOG_DEBUG, "%s: overflow nonce=%0.8x lastnonce=%0.8x", ztex->repr, nonce, lastnonce[i]);
				overflow = true;
			} else
				lastnonce[i] = nonce;
			nonce = htole32(nonce);
			if (!ztex_checkNonce(ztex, work, &hdata[i])) {
				thr->cgpu->hw_errors++;
				++hw_errors;
				continue;
			}
			for (j=0; j<=ztex->extraSolutions; j++) {
				nonce = hdata[i].goldenNonce[j];
				if (nonce > 0) {
					found = false;
					for (k = 0; k < backlog_max; k++) {
						if (backlog[k] == nonce) {
							found = true;
							break;
						}
					}
					if (!found) {
						backlog[backlog_p++] = nonce;
						if (backlog_p >= backlog_max)
							backlog_p = 0;
						nonce = le32toh(nonce);
						work->blk.nonce = 0xffffffff;
						if (test_nonce(work, nonce, false))
							submit_nonce(thr, work, nonce);
						applog(LOG_DEBUG, "%s: submitted %0.8x (from N%dE%d)", ztex->repr, nonce, i, j);
					}
				}
			}

		}

	}

	dclk_preUpdate(&ztex->dclk);

	if (!ztex_updateFreq(thr)) {
		// Something really serious happened, so mark this thread as dead!
		free(lastnonce);
		free(backlog);
		
		return -1;
	}

	applog(LOG_DEBUG, "%s: exit %1.8X", ztex->repr, noncecnt);

	work->blk.nonce = 0xffffffff;

	free(lastnonce);
	free(backlog);
	
	return noncecnt;
}

static void ztex_statline_before(char *buf, struct cgpu_info *cgpu)
{
	char before[] = "               ";
	if (cgpu->device_ztex) {
		const char *snString = (char*)cgpu->device_ztex->snString;
		size_t snStringLen = strlen(snString);
		if (snStringLen > 14)
			snStringLen = 14;
		memcpy(before, snString, snStringLen);
	}
	tailsprintf(buf, "%s| ", &before[0]);
}

static struct api_data*
get_ztex_api_extra_device_status(struct cgpu_info *ztex)
{
	struct api_data*root = NULL;
	struct libztex_device *ztexr = ztex->device_ztex;

	if (ztexr) {
		double frequency = ztexr->freqM1 * (ztexr->dclk.freqM + 1);
		root = api_add_freq(root, "Frequency", &frequency, true);
	}

	return root;
}

static bool ztex_prepare(struct thr_info *thr)
{
	struct timeval now;
	struct cgpu_info *cgpu = thr->cgpu;
	struct libztex_device *ztex = cgpu->device_ztex;

	gettimeofday(&now, NULL);
	get_datestamp(cgpu->init, &now);
	
	ztex_selectFpga(ztex);
	if (libztex_configureFpga(ztex) != 0) {
		libztex_resetFpga(ztex);
		ztex_releaseFpga(ztex);
		return false;
	}
	ztex->dclk.freqM = ztex->dclk.freqMaxM+1;;
	//ztex_updateFreq(thr);
	libztex_setFreq(ztex, ztex->dclk.freqMDefault);
	ztex_releaseFpga(ztex);
	applog(LOG_DEBUG, "%s: prepare", ztex->repr);
	return true;
}

static void ztex_shutdown(struct thr_info *thr)
{
	struct cgpu_info *cgpu = thr->cgpu;
	struct libztex_device *ztex = cgpu->device_ztex;
	
	if (!ztex)
		return;
	
	cgpu->device_ztex = NULL;
	if (ztex->root->numberOfFpgas > 1 && ztex->fpgaNum == 0)
		pthread_mutex_destroy(&ztex->mutex);
	applog(LOG_DEBUG, "%s: shutdown", ztex->repr);
	libztex_destroy_device(ztex);
}

static void ztex_disable(struct thr_info *thr)
{
	applog(LOG_ERR, "%s: Disabling!", thr->cgpu->device_ztex->repr);
	devices[thr->cgpu->device_id]->deven = DEV_DISABLED;
	ztex_shutdown(thr);
}

struct device_api ztex_api = {
	.dname = "ztex",
	.name = "ZTX",
	.api_detect = ztex_detect,
	.get_statline_before = ztex_statline_before,
	.get_api_extra_device_status = get_ztex_api_extra_device_status,
	.thread_init = ztex_prepare,
	.scanhash = ztex_scanhash,
	.thread_shutdown = ztex_shutdown,
};
