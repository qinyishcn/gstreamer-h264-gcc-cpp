/*-
* Copyright (c) 2017-2018 Razor, Inc.
*	All rights reserved.
*
* See the file LICENSE for redistribution information.
*/

#include "estimator_common.h"

void feedback_msg_encode(bin_stream_t* strm, feedback_msg_t* msg)
{
	int i;

	mach_uint8_write(strm, msg->flag);

	if ((msg->flag & remb_msg) == remb_msg)
		mach_uint32_write(strm, msg->remb);

	if ((msg->flag & loss_info_msg) == loss_info_msg){
		mach_uint8_write(strm, msg->fraction_loss);
		mach_int32_write(strm, msg->packet_num);
	}

	if ((msg->flag & proxy_ts_msg) == proxy_ts_msg){
		mach_int64_write(strm, msg->base_seq);
		mach_int64_write(strm, msg->min_ts);

		mach_uint8_write(strm, msg->samples_num);
		for (i = 0; i < msg->samples_num; ++i){
			mach_uint16_write(strm, msg->samples[i].seq);
			mach_uint16_write(strm, (uint16_t)(msg->samples[i].ts - msg->min_ts)); /*此处timestamp之差不可能大于65535，因为在proxy里面只保留500毫秒的报文记录*/
		}
	}
}

int feedback_msg_decode(bin_stream_t* strm, feedback_msg_t* msg)
{
	int i;
	uint16_t delta_ts;

	if (strm == NULL || msg == NULL)
		return -1;

	memset(msg, 0, sizeof(*msg));
	if (bin_stream_available_read(strm) < 1)
		return -1;
	mach_uint8_read(strm, &msg->flag);

	if ((msg->flag & remb_msg) == remb_msg){
		if (bin_stream_available_read(strm) < 4)
			return -1;
		mach_uint32_read(strm, &msg->remb);
	}

	if ((msg->flag & loss_info_msg) == loss_info_msg){
		if (bin_stream_available_read(strm) < 1)
			return -1;
		mach_uint8_read(strm, &msg->fraction_loss);
		if (bin_stream_available_read(strm) < 4)
			return -1;
		mach_int32_read(strm, &msg->packet_num);
	}

	if ((msg->flag & proxy_ts_msg) == proxy_ts_msg){
		if (bin_stream_available_read(strm) < 8)
			return -1;
		mach_int64_read(strm, &msg->base_seq);
		if (bin_stream_available_read(strm) < 8)
			return -1;
		mach_int64_read(strm, &msg->min_ts);

		if (bin_stream_available_read(strm) < 1)
			return -1;
		mach_uint8_read(strm, &msg->samples_num);
		if (msg->samples_num > MAX_FEELBACK_COUNT)
			return -1;
		if (bin_stream_available_read(strm) < msg->samples_num * 4)
			return -1;

		for (i = 0; i < msg->samples_num; ++i){
			mach_uint16_read(strm, &msg->samples[i].seq);
			mach_uint16_read(strm, &delta_ts);
			if (msg->min_ts > INT64_MAX - (int64_t)delta_ts)
				return -1;
			msg->samples[i].ts = msg->min_ts + delta_ts;
		}
	}

	return 0;
}




