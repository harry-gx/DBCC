/* @brief Convert the Abstract Syntax Tree generated by mpc for the DBC file
 * into an equivalent XML file.
 * @copyright Richard James Howe (2018)
 * @license MIT **/
#include "2bsm.h"
#include "util.h"
#include <assert.h>
#include <time.h>

/* Add: <?xml-stylesheet type="text/xsl" href="yourxsl.xsl"?> */

#define BSM_PREFIX "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n\
<beSTORM Version=\"1.2\">\n\
\t<GeneratorOptSettings >\n\
\t\t<BT FactoryDefined=\"1\" MaxBytesToGenerate=\"8\" FactoryType=\"Binary\" />\n\
\t</GeneratorOptSettings>\n\
\t<ModuleSettings>\n\
\t\t<M Name=\"CAN\">\n\
\t\t\t<P Name=\"CAN Protocol\">\n\
\t\t\t\t<SC Name=\"CAN Sequence\">\n\
\t\t\t\t\t<SP Name=\"CAN Open\" Library=\"CAN Interface.dll\" Procedure=\"OpenDevice\">\n\
\t\t\t\t\t\t<S Name=\"IPAddress\">\n\
\t\t\t\t\t\t\t<EV Name=\"IPAddress\" Description=\"CAN IP Address\" ASCIIValue=\"&lt;CAN Device&gt;\" Required=\"1\" />\n\
\t\t\t\t\t\t</S>\n\
\t\t\t\t\t\t<S Name=\"Port\">\n\
\t\t\t\t\t\t\t<EV Name=\"Port\" Description=\"CAN Port\" ASCIIValue=\"0\" Required=\"1\" Comment=\"Should be either 0, 1, 2, or 3\"/>\n\
\t\t\t\t\t\t</S>\n\
\t\t\t\t\t</SP>\n\
\t\t\t\t\t<SP Name=\"CAN SetGlobals\" Library=\"CAN Interface.dll\" Procedure=\"SetGlobals\">\n\
\t\t\t\t\t\t<S Name=\"HANDLE\">\n\
\t\t\t\t\t\t\t<PC Name=\"CAN\" ConditionedName=\"CAN Open\" Parameter=\"HANDLE\" />\n\
\t\t\t\t\t\t</S>\n\
\t\t\t\t\t\t<S Name=\"Baudrate\">\n\
\t\t\t\t\t\t\t<EV Name=\"Baudrate\" Description=\"Baudrate\" ASCIIValue=\"250000\" Required=\"1\" Comment=\"Should be either '10000', '20000', '50000', '62500', '100000', '125000', '250000', '500000', '800000', or '1000000'\"/>\n\
\t\t\t\t\t\t</S>\n\
\t\t\t\t\t</SP>\n\
\n\
\t\t\t\t\t<SE Name=\"Messages\">\n\
"

#define BSM_MESSAGE_PREFIX "\n\
\t\t\t\t\t\t<SP Name=\"CAN Send (%s - %lu)\" Library=\"CAN Interface.dll\" Procedure=\"Write\">\n\
\t\t\t\t\t\t\t<S Name=\"HANDLE\">\n\
\t\t\t\t\t\t\t\t<PC Name=\"HANDLE\" ConditionedName=\"CAN Open\" Parameter=\"HANDLE\" />\n\
\t\t\t\t\t\t\t</S>\n\
\t\t\t\t\t\t\t<S Name=\"Identifier\">\n\
\t\t\t\t\t\t\t\t<C Name=\"Identifier\">%lu</C>\n\
\t\t\t\t\t\t\t</S>\n\
\t\t\t\t\t\t\t<S ParamName=\"Data\" Name=\"Message\">\n\
\t\t\t\t\t\t\t\t<BC Name=\"Message Bits\" PaddingSize=\"%d\" PaddingBit=\"0\">\n\
"

#define BSM_MESSAGE_SUFFIX "\t\t\t\t\t\t\t\t</BC>\n\
\t\t\t\t\t\t\t</S>\n\
\t\t\t\t\t\t</SP>\n\
"

#define BSM_SUFFIX "\n\
\t\t\t\t\t</SE>\n\
\n\
\t\t\t\t\t<SP Name=\"CAN Close\" Library=\"CAN Interface.dll\" Procedure=\"CloseDevice\">\n\
\t\t\t\t\t\t<S Name=\"HANDLE\">\n\
\t\t\t\t\t\t\t<PC Name=\"CAN\" ConditionedName=\"CAN Open\" Parameter=\"HANDLE\" />\n\
\t\t\t\t\t\t</S>\n\
\t\t\t\t\t</SP>\n\
\t\t\t\t</SC>\n\
\t\t\t</P>\n\
\t\t</M>\n\
\t</ModuleSettings>\n\
</beSTORM>\n\
"

static int indent(FILE * o, unsigned depth)
{
	assert(o);
	while (depth--)
		if (fputc('\t', o) != '\t')
			return -1;
	return 0;
}

static int comment(FILE * o, unsigned depth, const char *fmt, ...)
{
	assert(o);
	assert(fmt);
	va_list args;
	assert(o && fmt);
	errno = 0;
	if (indent(o, depth) < 0)
		goto warn;
	if (fputs("<!-- ", o) < 0)
		goto warn;
	assert(fmt);
	va_start(args, fmt);
	int r = vfprintf(o, fmt, args);
	va_end(args);
	if (r < 0)
		goto warn;
	if (fputs(" -->\n", o) < 0)
		goto warn;
	return 0;
 warn:
	warning("XML comment generation, problem writing to FILE* <%p>: %s", o,
		emsg());
	return -1;
}

static int signal2bsm(signal_t * sig, FILE * o, unsigned depth)
{
	assert(sig);
	assert(o);
	UNUSED(depth);

	if (sig->bit_length > 16) { /* We need to split it into two, because we assume a <BB> is a 16 bit element (0xXX 0x00) */
		fprintf(o, "\t\t\t\t\t\t\t\t\t<BB Name=\"%s (LSB)\" Bits=\"0\" Size=\"%d\" />\n", sig->name, 16);
		fprintf(o, "\t\t\t\t\t\t\t\t\t<BB Name=\"%s (MSB)\" Bits=\"0\" Size=\"%d\" />\n", sig->name, sig->bit_length - 16);
} else {
		fprintf(o, "\t\t\t\t\t\t\t\t\t<BB Name=\"%s\" Bits=\"0\" Size=\"%d\" />\n", sig->name, sig->bit_length);
	}

	return 0;
}

static int msg2bsm(can_msg_t * msg, FILE * o, unsigned depth)
{
	assert(msg);
	assert(o);
	indent(o, depth);

	unsigned last_bit = 0;	/* Detect gaps between signals */

	unsigned int padding_size = 0;	/* Find how much we need to pad the data to, 8, 16, 24, or 32 */
	for (size_t i = 0; i < msg->signal_count; i++) {
		signal_t *sig = msg->sigs[i];

		if (last_bit < sig->start_bit) {
			/* We have a void, create a fake signal of UNKNOWN in the middle */
			padding_size += sig->start_bit - last_bit;

			last_bit = sig->start_bit;
		}

		padding_size += sig->bit_length;
		last_bit = sig->start_bit + sig->bit_length;
	}

	if (padding_size <= 8) {
		padding_size = 8;
	} else if (padding_size <= 16) {
		padding_size = 16;
	} else if (padding_size <= 24) {
		padding_size = 24;
	} else if (padding_size <= 32) {
		padding_size = 32;
	}

	fprintf(o, BSM_MESSAGE_PREFIX, msg->name, msg->id, msg->id, padding_size);

	last_bit = 0;
	signal_t *multiplexor = NULL;
	for (size_t i = 0; i < msg->signal_count; i++) {
		signal_t *sig = msg->sigs[i];
		if (sig->is_multiplexor) {
			if (multiplexor) {
				error ("multiple multiplexor values detected (only one per CAN msg is allowed) for %s", msg->name);
				return -1;
			}
			multiplexor = sig;
			continue;
		}
		if (sig->is_multiplexed)
			continue;

		if (last_bit < sig->start_bit) {
			/* We have a void, create a fake signal of UNKNOWN in the middle */
			char szUNKNOWN[] = "UNKNOWN";
			char szUNITS[] = "";
			signal_t unknownsig = { 0 };
			unknownsig.name = szUNKNOWN;
			unknownsig.units = szUNITS;
			unknownsig.start_bit = last_bit;
			unknownsig.bit_length = sig->start_bit - last_bit;

			if (signal2bsm(&unknownsig, o, depth + 1) < 0)
				return -1;

			last_bit = sig->start_bit;
		}
		if (signal2bsm(sig, o, depth + 1) < 0)
			return -1;

		last_bit = sig->start_bit + sig->bit_length;
	}
	if (fprintf(o, BSM_MESSAGE_SUFFIX) < 0)
		return -1;
	return 0;
}

int dbc2bsm(dbc_t * dbc, FILE * output, bool use_time_stamps)
{
	assert(dbc);
	assert(output);
	time_t rawtime = time(NULL);
	struct tm *timeinfo = localtime(&rawtime);

	comment(output, 0, "Generated by dbcc (see https://github.com/howerj/dbcc)");
	fprintf(output, BSM_PREFIX);

	if (use_time_stamps)
		comment(output, 0, "Generated on: %s", asctime(timeinfo));

	for (size_t i = 0; i < dbc->message_count; i++) {
		if (msg2bsm(dbc->messages[i], output, 1) < 0) {
			return -1;
		}
	}

	fprintf(output, BSM_SUFFIX);

	return 0;
}
