/*
 * voice.c - /dev/voiceN wrapper implementation
 */

#include <comatose/voice.h>
#include <comatose/tapi_defs.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int find_major(const char *name)
{
	FILE *f = fopen("/proc/devices", "r");
	if (!f)
		return -1;
	char line[128];
	int major = -1;
	while (fgets(line, sizeof(line), f)) {
		int num;
		char devname[64];
		if (sscanf(line, " %d %63s", &num, devname) == 2 &&
		    strcmp(devname, name) == 0) {
			major = num;
			break;
		}
	}
	fclose(f);
	return major;
}

int voice_open(int session_id)
{
	if (session_id < 0 || session_id >= 16)
		return -1;

	char path[32];
	snprintf(path, sizeof(path), "/dev/voice%d", session_id);

	/* ensure device node exists */
	struct stat st;
	if (stat(path, &st) != 0) {
		int major = find_major("voice");
		if (major < 0)
			major = TAPI_MAJOR_VOICE;
		if (mknod(path, S_IFCHR | 0660, makedev(major, session_id)) < 0 &&
		    errno != EEXIST)
			return -1;
	}

	return open(path, O_RDWR);
}

void voice_close(int fd)
{
	if (fd < 0)
		return;
	ioctl(fd, VOICE_IOCSTOP_SESSION);
	close(fd);
}

comatose_result_t voice_set_codec(int fd, const rtp_session_config *cfg)
{
	if (fd < 0 || !cfg)
		return COMATOSE_ERR_INVALID;
	if (ioctl(fd, VOICE_IOCSETCODEC, cfg) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

comatose_result_t voice_stop(int fd)
{
	if (fd < 0)
		return COMATOSE_ERR_INVALID;
	if (ioctl(fd, VOICE_IOCSTOP_SESSION) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

void voice_config_g711u(rtp_session_config *cfg, int line_id, int session_id)
{
	memset(cfg, 0, sizeof(*cfg));

	cfg->codec.tx_pt = RTP_PT_G711U;
	cfg->codec.tx_pt_event = 0xff;
	cfg->codec.rx_pt_event = 0xff;
	cfg->codec.duration = 20;
	cfg->codec.opts = RTP_CODEC_OPT_NONE;
	strncpy(cfg->codec.CodecStr, "pcmu/8000",
	        sizeof(cfg->codec.CodecStr) - 1);

	cfg->codec.rx_list[0].rx_pt = RTP_PT_G711U;
	strncpy(cfg->codec.rx_list[0].CodecStr, "pcmu/8000",
	        sizeof(cfg->codec.rx_list[0].CodecStr) - 1);
	for (int i = 1; i < VOICE_MAX_CODECS; i++)
		cfg->codec.rx_list[i].rx_pt = (char)0xff;

	cfg->opts = RTP_OPT_NONE;
	cfg->audio_mode = RTP_MODE_ACTIVE;
	cfg->lib_rtp_mode = RTP_APP_VOIP_USER;
	cfg->voip_line_id = line_id;
	cfg->session_id = session_id;
	cfg->SymmRTPTxPktCnt = 10;
}
