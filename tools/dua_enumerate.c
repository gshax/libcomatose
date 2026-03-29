/*
 * dua_enumerate - connect to the DUA service and discover available units
 *
 * sends EnumUT, EnumUE, and EnumUEParam commands to list what the CSS
 * firmware exposes. this is the first real test of the COMA/DUA transport.
 *
 * usage: dua_enumerate
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <string.h>

static void hexdump(const void *data, size_t len)
{
	const uint8_t *p = data;
	for (size_t i = 0; i < len; i++) {
		if (i > 0 && (i % 16) == 0)
			printf("\n  ");
		printf("%02x ", p[i]);
	}
	printf("\n");
}

/*
 * send a raw DUA enumeration command and print the response.
 * this uses the low-level API since the enumeration commands use
 * the string-based protocol and we need to inspect raw responses.
 */
static int enumerate_unit_types(dua_session_t *sess)
{
	uint8_t buf[DUA_MSG_BUF_SIZE];
	uint8_t resp[1024];

	printf("=== enumerating unit types (cmd 0x7b) ===\n");

	/*
	 * EnumUT message: cmd=0x7b, params[4]=unitTypeIndex
	 * iterate until we get an error response
	 */
	for (int i = 0; i < 8; i++) {
		size_t off = dua_msg_init(buf, sizeof(buf), i + 100,
		                          DUA_CMD_SC_ENUM_UT, 1);
		off = dua_msg_pack_u32(buf, off, (uint32_t)i);

		size_t resp_len = sizeof(resp);
		comatose_result_t ret = dua_transact(sess, buf, off,
		                                     resp, &resp_len, 3000);
		if (ret == COMATOSE_ERR_TIMEOUT) {
			printf("  [%d] timeout\n", i);
			break;
		}
		if (ret != COMATOSE_OK) {
			printf("  [%d] error: %d\n", i, ret);
			break;
		}

		printf("  unit type %d: %zu bytes response:\n  ", i, resp_len);
		hexdump(resp, resp_len);
	}

	return 0;
}

static int enumerate_unit_elements(dua_session_t *sess, int unit_type)
{
	uint8_t buf[DUA_MSG_BUF_SIZE];
	uint8_t resp[1024];

	printf("\n=== enumerating elements for unit type %d (cmd 0x7c) ===\n",
	       unit_type);

	dua_uid_t uid = DUA_UID_MAKE(unit_type, 0);

	for (int i = 0; i < 32; i++) {
		size_t off = dua_msg_init(buf, sizeof(buf), i + 200,
		                          DUA_CMD_SC_ENUM_UE, 2);
		off = dua_msg_pack_u32(buf, off, (uint32_t)(int16_t)uid);
		off = dua_msg_pack_u32(buf, off, (uint32_t)i);

		size_t resp_len = sizeof(resp);
		comatose_result_t ret = dua_transact(sess, buf, off,
		                                     resp, &resp_len, 3000);
		if (ret == COMATOSE_ERR_TIMEOUT) {
			printf("  [%d] timeout\n", i);
			break;
		}
		if (ret != COMATOSE_OK) {
			printf("  [%d] error: %d\n", i, ret);
			break;
		}

		printf("  elem %d: %zu bytes response:\n  ", i, resp_len);
		hexdump(resp, resp_len);
	}

	return 0;
}

int main(void)
{
	printf("connecting to DUA service...\n");

	dua_session_t *sess = dua_open();
	if (!sess) {
		fprintf(stderr, "dua_open failed (is the CSS loaded? do you have CAP_SYS_ADMIN?)\n");
		return 1;
	}

	printf("connected! sending ApplInit...\n");
	comatose_result_t ret = dua_appl_init(sess);
	if (ret != COMATOSE_OK) {
		fprintf(stderr, "dua_appl_init failed: %d\n", ret);
		dua_close(sess);
		return 1;
	}
	printf("ApplInit OK\n\n");

	enumerate_unit_types(sess);

	/* enumerate elements for each known unit type */
	enumerate_unit_elements(sess, DUA_UT_SPVOIPNDA);
	enumerate_unit_elements(sess, DUA_UT_FXS);

	dua_close(sess);
	printf("\ndone.\n");
	return 0;
}
