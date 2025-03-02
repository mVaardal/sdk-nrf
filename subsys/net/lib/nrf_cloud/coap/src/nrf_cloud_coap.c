/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/coap.h>
#include <date_time.h>
#include <dk_buttons_and_leds.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_agps.h>
#include <net/nrf_cloud_pgps.h>
#include <net/nrf_cloud_coap.h>
#include "nrf_cloud_coap_transport.h"
#include "nrf_cloud_codec_internal.h"
#include "nrf_cloud_mem.h"
#include "coap_codec.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nrf_cloud_coap, CONFIG_NRF_CLOUD_COAP_LOG_LEVEL);

#define MAX_COAP_PAYLOAD_SIZE (CONFIG_COAP_CLIENT_BLOCK_SIZE - \
			       CONFIG_COAP_CLIENT_MESSAGE_HEADER_SIZE)

static int64_t get_ts(void)
{
	int64_t ts;
	int err;

	err = date_time_now(&ts);
	if (err) {
		LOG_ERR("Error getting time: %d", err);
		ts = 0;
	}
	return ts;
}

#if defined(CONFIG_NRF_CLOUD_AGPS)
static int agps_err;

static void get_agps_callback(int16_t result_code,
			      size_t offset, const uint8_t *payload, size_t len,
			      bool last_block, void *user_data)
{
	struct nrf_cloud_rest_agps_result *result = user_data;

	if (!result) {
		LOG_ERR("Cannot process result");
		agps_err = -EINVAL;
		return;
	}
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		agps_err = result_code;
		return;
	}
	if (((offset + len) <= result->buf_sz) && result->buf && payload) {
		memcpy(&result->buf[offset], payload, len);
		result->agps_sz += len;
	} else {
		agps_err = -EOVERFLOW;
		return;
	}
	if (last_block) {
		agps_err = 0;
	}
}

int nrf_cloud_coap_agps_data_get(struct nrf_cloud_rest_agps_request const *const request,
				 struct nrf_cloud_rest_agps_result *result)
{
	__ASSERT_NO_MSG(request != NULL);
	__ASSERT_NO_MSG(result != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}

	static uint8_t buffer[64];
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_agps_encode(request, buffer, &len,
				     COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode A-GPS request: %d", err);
		return err;
	}

	result->agps_sz = 0;
	err = nrf_cloud_coap_fetch("loc/agps", NULL,
				   buffer, len, COAP_CONTENT_FORMAT_APP_CBOR,
				   COAP_CONTENT_FORMAT_APP_CBOR, true, get_agps_callback,
				   result);
	if (!err && !agps_err) {
		LOG_INF("Got A-GPS data");
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for A-GPS data");
	} else {
		LOG_ERR("Error getting A-GPS; agps_err:%d, err:%d", agps_err, err);
		err = agps_err;
	}

	return err;
}
#endif /* CONFIG_NRF_CLOUD_AGPS */

#if defined(CONFIG_NRF_CLOUD_PGPS)
static int pgps_err;

static void get_pgps_callback(int16_t result_code,
			      size_t offset, const uint8_t *payload, size_t len,
			      bool last_block, void *user)
{
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		pgps_err = result_code;
	} else {
		pgps_err = coap_codec_pgps_resp_decode(user, payload, len,
						       COAP_CONTENT_FORMAT_APP_CBOR);
	}
}

int nrf_cloud_coap_pgps_url_get(struct nrf_cloud_rest_pgps_request const *const request,
				struct nrf_cloud_pgps_result *file_location)
{
	__ASSERT_NO_MSG(request != NULL);
	__ASSERT_NO_MSG(file_location != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}

	static uint8_t buffer[64];
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_pgps_encode(request, buffer, &len,
				     COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode P-GPS request: %d", err);
		return err;
	}

	err = nrf_cloud_coap_fetch("loc/pgps", NULL,
				   buffer, len, COAP_CONTENT_FORMAT_APP_CBOR,
				   COAP_CONTENT_FORMAT_APP_CBOR, true, get_pgps_callback,
				   file_location);
	if (!err && !pgps_err) {
		LOG_INF("Got P-GPS file location");
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for P-GPS file location");
	} else {
		LOG_ERR("Error getting P-GPS file location; pgps_err:%d, err:%d", pgps_err, err);
		err = pgps_err;
	}

	return err;
}
#endif /* CONFIG_NRF_CLOUD_PGPS */

int nrf_cloud_coap_sensor_send(const char *app_id, double value, int64_t ts_ms)
{
	__ASSERT_NO_MSG(app_id != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}
	int64_t ts = (ts_ms == NRF_CLOUD_NO_TIMESTAMP) ? get_ts() : ts_ms;
	static uint8_t buffer[32];
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_sensor_encode(app_id, value, ts, buffer, &len,
				       COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode sensor data: %d", err);
		return err;
	}
	err = nrf_cloud_coap_post("msg/d2c", NULL, buffer, len,
				  COAP_CONTENT_FORMAT_APP_CBOR, false, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}
	return err;
}

int nrf_cloud_coap_location_send(const struct nrf_cloud_gnss_data *gnss)
{
	__ASSERT_NO_MSG(gnss != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}
	int64_t ts = (gnss->ts_ms == NRF_CLOUD_NO_TIMESTAMP) ? get_ts() : gnss->ts_ms;
	static uint8_t buffer[64];
	size_t len = sizeof(buffer);
	int err;

	if (gnss->type != NRF_CLOUD_GNSS_TYPE_PVT) {
		LOG_ERR("Only PVT format is supported");
		return -ENOTSUP;
	}
	err = coap_codec_pvt_encode("GNSS", &gnss->pvt, ts, buffer, &len,
				    COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode GNSS PVT data: %d", err);
		return err;
	}
	err = nrf_cloud_coap_post("msg/d2c", NULL, buffer, len,
				  COAP_CONTENT_FORMAT_APP_CBOR, false, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send POST request: %d", err);
	}
	return err;
}

static int loc_err;

static void get_location_callback(int16_t result_code,
				  size_t offset, const uint8_t *payload, size_t len,
				  bool last_block, void *user)
{
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		loc_err = result_code;
	} else {
		loc_err = coap_codec_ground_fix_resp_decode(user, payload, len,
							    COAP_CONTENT_FORMAT_APP_CBOR);
	}
}

int nrf_cloud_coap_location_get(struct nrf_cloud_rest_location_request const *const request,
				struct nrf_cloud_location_result *const result)
{
	__ASSERT_NO_MSG(request != NULL);
	__ASSERT_NO_MSG((request->cell_info != NULL) || (request->wifi_info != NULL));
	__ASSERT_NO_MSG(result != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}
	static uint8_t buffer[256];
	size_t len = sizeof(buffer);
	int err;

	err = coap_codec_ground_fix_req_encode(request->cell_info, request->wifi_info, buffer, &len,
					       COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		LOG_ERR("Unable to encode location data: %d", err);
		return err;
	}
	err = nrf_cloud_coap_fetch("loc/ground-fix", NULL, buffer, len,
				   COAP_CONTENT_FORMAT_APP_CBOR,
				   COAP_CONTENT_FORMAT_APP_CBOR, true,
				   get_location_callback, result);

	if (!err && !loc_err) {
		LOG_DBG("Location: %d, %.12g, %.12g, %d", result->type,
			result->lat, result->lon, result->unc);
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for location");
	} else {
		LOG_ERR("Error getting location; loc_err:%d, err:%d", loc_err, err);
		err = loc_err;
	}

	return err;
}

static int fota_err;

static void get_fota_callback(int16_t result_code,
			      size_t offset, const uint8_t *payload, size_t len,
			      bool last_block, void *user)
{
	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		fota_err = result_code;
	} else if (payload && len) {
		LOG_DBG("Got FOTA response: %.*s", len, (const char *)payload);
		fota_err = coap_codec_fota_resp_decode(user, payload, len,
						       COAP_CONTENT_FORMAT_APP_JSON);
	} else {
		fota_err = -ENOMSG;
	}
}

int nrf_cloud_coap_fota_job_get(struct nrf_cloud_fota_job_info *const job)
{
	__ASSERT_NO_MSG(job != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}
	int err;

	job->type = NRF_CLOUD_FOTA_TYPE__INVALID;

	err = nrf_cloud_coap_get("fota/exec/current", NULL, NULL, 0,
				 COAP_CONTENT_FORMAT_APP_CBOR,
				 COAP_CONTENT_FORMAT_APP_JSON, true, get_fota_callback, job);

	if (!err && !fota_err) {
		LOG_INF("FOTA job received; type:%d, id:%s, host:%s, path:%s, size:%d",
			job->type, job->id, job->host, job->path, job->file_size);
	} else if (!err && (fota_err == COAP_RESPONSE_CODE_NOT_FOUND)) {
		LOG_INF("No pending FOTA job");
		err = -ENOMSG;
	} else if (err == -EAGAIN) {
		LOG_ERR("Timeout waiting for FOTA job");
	} else if (err < 0) {
		LOG_ERR("Error getting current FOTA job:%d", err);
	} else {
		LOG_RESULT_CODE_ERR("Unexpected CoAP response code getting current FOTA job; rc:",
				    fota_err);
		err = fota_err;
	}
	return err;
}

void nrf_cloud_coap_fota_job_free(struct nrf_cloud_fota_job_info *const job)
{
	nrf_cloud_fota_job_free(job);
}

int nrf_cloud_coap_fota_job_update(const char *const job_id,
	const enum nrf_cloud_fota_status status, const char * const details)
{
	__ASSERT_NO_MSG(job_id != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}

	int ret;
	struct nrf_cloud_fota_job_update update;

	ret = nrf_cloud_fota_job_update_create(NULL, job_id, status, details, &update);
	if (ret) {
		LOG_ERR("Error creating FOTA job update structure: %d", ret);
		return ret;
	}
	ret = nrf_cloud_coap_patch(update.url, NULL, update.payload, strlen(update.payload),
				   COAP_CONTENT_FORMAT_APP_JSON, true, NULL, NULL);

	nrf_cloud_fota_job_update_free(&update);

	return ret;
}

struct get_shadow_data  {
	char *buf;
	size_t buf_len;
} get_shadow_data;
static int shadow_err;

static void get_shadow_callback(int16_t result_code,
				size_t offset, const uint8_t *payload, size_t len,
				bool last_block, void *user)
{
	struct get_shadow_data *data = (struct get_shadow_data *)user;

	if (result_code != COAP_RESPONSE_CODE_CONTENT) {
		shadow_err = result_code;
	} else {
		int cpy_len = MIN(data->buf_len - 1, len);

		if (cpy_len < len) {
			LOG_WRN("Shadow truncated from %zd to %zd characters.",
				len, cpy_len);
		}
		shadow_err = 0;
		if (cpy_len) {
			memcpy(data->buf, payload, cpy_len);
		}
		data->buf[cpy_len] = '\0';
	}
}

int nrf_cloud_coap_shadow_get(char *buf, size_t buf_len, bool delta)
{
	__ASSERT_NO_MSG(buf != NULL);
	__ASSERT_NO_MSG(buf_len != 0);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}

	get_shadow_data.buf = buf;
	get_shadow_data.buf_len = buf_len;

	return nrf_cloud_coap_get("state", delta ? NULL : "delta=false", NULL, 0,
				  0, COAP_CONTENT_FORMAT_APP_JSON, true, get_shadow_callback,
				  &get_shadow_data);
}

int nrf_cloud_coap_shadow_state_update(const char * const shadow_json)
{
	__ASSERT_NO_MSG(shadow_json != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}

	return nrf_cloud_coap_patch("state", NULL, (uint8_t *)shadow_json,
				    strlen(shadow_json),
				    COAP_CONTENT_FORMAT_APP_JSON, true, NULL, NULL);
}

int nrf_cloud_coap_shadow_device_status_update(const struct nrf_cloud_device_status
					       *const dev_status)
{
	__ASSERT_NO_MSG(dev_status != NULL);
	if (!nrf_cloud_coap_is_connected()) {
		return -EACCES;
	}

	int ret;
	struct nrf_cloud_data data_out;

	ret = nrf_cloud_shadow_dev_status_encode(dev_status, &data_out, false, false);
	if (ret) {
		LOG_ERR("Failed to encode device status, error: %d", ret);
		return ret;
	}

	ret = nrf_cloud_coap_shadow_state_update(data_out.ptr);
	if (ret) {
		LOG_ERR("Failed to update device shadow, error: %d", ret);
	}

	nrf_cloud_device_status_free(&data_out);

	return ret;
}

int nrf_cloud_coap_shadow_service_info_update(const struct nrf_cloud_svc_info * const svc_inf)
{
	if (svc_inf == NULL) {
		return -EINVAL;
	}

	const struct nrf_cloud_device_status dev_status = {
		.modem = NULL,
		.svc = (struct nrf_cloud_svc_info *)svc_inf
	};

	return nrf_cloud_coap_shadow_device_status_update(&dev_status);
}
