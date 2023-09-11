#include "init.h"
#include "translate.h"

static const t_unicode cp855_c1_chars[] = {
0x0452, 0x0402, 0x0453, 0x0403, 0x0451, 0x0401, 0x0454, 0x0404, /* 0x80-0x87 */
0x0455, 0x0405, 0x0456, 0x0406, 0x0457, 0x0407, 0x0458, 0x0408, /* 0x88-0x8F */
0x0459, 0x0409, 0x045a, 0x040a, 0x045b, 0x040b, 0x045c, 0x040c, /* 0x90-0x97 */
0x045e, 0x040e, 0x045f, 0x040f, 0x044e, 0x042e, 0x044a, 0x042a, /* 0x98-0x9F */
};
struct char_set cp855_c1 = {
	1,
	CHARS(cp855_c1_chars),
	0, "", 0, 32,
};

static const t_unicode cp855_g1_chars[] = {
0x0430, 0x0410, 0x0431, 0x0411, 0x0446, 0x0426, 0x0434, 0x0414, /* 0xA0-0xA7 */
0x0435, 0x0415, 0x0444, 0x0424, 0x0433, 0x0413, 0x00ab, 0x00bb, /* 0xA8-0xAF */
0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x0445, 0x0425, 0x0438, /* 0xB0-0xB7 */
0x0418, 0x2563, 0x2551, 0x2557, 0x255d, 0x0439, 0x0419, 0x2510, /* 0xB8-0xBF */
0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x043a, 0x041a, /* 0xC0-0xC7 */
0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x00a4, /* 0xC8-0xCF */
0x043b, 0x041b, 0x043c, 0x041c, 0x043d, 0x041d, 0x043e, 0x041e, /* 0xD0-0xD7 */
0x043f, 0x2518, 0x250c, 0x2588, 0x2584, 0x041f, 0x044f, 0x2580, /* 0xD8-0xDF */
0x042f, 0x0440, 0x0420, 0x0441, 0x0421, 0x0442, 0x0422, 0x0443, /* 0xE0-0xE7 */
0x0423, 0x0436, 0x0416, 0x0432, 0x0412, 0x044c, 0x042c, 0x2116, /* 0xE8-0xEF */
0x00ad, 0x044b, 0x042b, 0x0437, 0x0417, 0x0448, 0x0428, 0x044d, /* 0xF0-0xF7 */
0x042d, 0x0449, 0x0429, 0x0447, 0x0427, 0x00a7, 0x25a0, 0x00a0, /* 0xF8-0xFF */
};
struct char_set cp855_g1 = {
	1,
	CHARS(cp855_g1_chars),
	0, "", 0, 96,
};

struct char_set cp855 = {
	.c0 = &ibm_ascii_c0,
	.g0 = &ibm_ascii_g0,
	.c1 = &cp855_c1,
	.g1 = &cp855_g1,
	.names = { "cp855", 0 }
};

CONSTRUCTOR(static void init(void))
{
	register_charset(&cp855);
}
