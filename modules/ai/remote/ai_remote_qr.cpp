/**************************************************************************/
/*  ai_remote_qr.cpp                                                      */
/**************************************************************************/
/* Data encoding, Reed-Solomon over GF(256), block interleaving, function  */
/* patterns, data masking and format/version information.                  */
/*                                                                        */
/* Only what the pairing URL needs: byte mode, error correction level M,   */
/* versions 1-10. Everything is table-driven from ISO/IEC 18004.           */
/**************************************************************************/

#include "ai_remote_qr.h"

#include "core/math/math_funcs.h" // Math::abs

// Versions 1-10 only; anything larger would need more of the standard's
// tables than the pairing URL will ever use.
static const int QR_MAX_VERSION = 10;

// Per version (level M): error correction codewords per block, and the block
// layout. Versions 8-10 split into two groups whose blocks differ in length by
// one data codeword.
struct QRVersionSpec {
	uint8_t ec_per_block;
	uint8_t group1_blocks;
	uint8_t group1_data;
	uint8_t group2_blocks;
	uint8_t group2_data;
};

static const QRVersionSpec QR_VERSION_SPECS[QR_MAX_VERSION + 1] = {
	{ 0, 0, 0, 0, 0 }, // unused, versions are 1-based
	{ 10, 1, 16, 0, 0 },
	{ 16, 1, 28, 0, 0 },
	{ 26, 1, 44, 0, 0 },
	{ 18, 2, 32, 0, 0 },
	{ 24, 2, 43, 0, 0 },
	{ 16, 4, 27, 0, 0 },
	{ 18, 4, 31, 0, 0 },
	{ 22, 2, 38, 2, 39 },
	{ 22, 3, 36, 2, 37 },
	{ 26, 4, 43, 1, 44 },
};

// Alignment pattern centre coordinates. Every combination of two coordinates
// is a centre, except the three that would land inside a finder pattern.
static const uint8_t QR_ALIGNMENT_COUNT[QR_MAX_VERSION + 1] = { 0, 0, 2, 2, 2, 2, 2, 3, 3, 3, 3 };
static const uint8_t QR_ALIGNMENT_POSITIONS[QR_MAX_VERSION + 1][3] = {
	{ 0, 0, 0 },
	{ 0, 0, 0 },
	{ 6, 18, 0 },
	{ 6, 22, 0 },
	{ 6, 26, 0 },
	{ 6, 30, 0 },
	{ 6, 34, 0 },
	{ 6, 22, 38 },
	{ 6, 24, 42 },
	{ 6, 26, 46 },
	{ 6, 28, 50 },
};

// Penalty weights for the four mask evaluation rules.
static const int QR_PENALTY_N1 = 3;
static const int QR_PENALTY_N2 = 3;
static const int QR_PENALTY_N3 = 40;
static const int QR_PENALTY_N4 = 10;

/**************************************************************************/
/* GF(256) arithmetic                                                     */
/**************************************************************************/

struct QRGaloisTables {
	uint8_t exp[512] = {};
	uint8_t log[256] = {};

	QRGaloisTables() {
		int x = 1;
		for (int i = 0; i < 255; i++) {
			exp[i] = (uint8_t)x;
			log[x] = (uint8_t)i;
			x <<= 1;
			if (x & 0x100) {
				x ^= 0x11D; // the primitive polynomial QR uses for GF(256)
			}
		}
		// Doubled so that log[a] + log[b] can index it without wrapping by hand.
		for (int i = 255; i < 512; i++) {
			exp[i] = exp[i - 255];
		}
	}
};

static const QRGaloisTables &_qr_gf() {
	static QRGaloisTables tables;
	return tables;
}

static uint8_t _qr_gf_mul(uint8_t p_a, uint8_t p_b) {
	if (p_a == 0 || p_b == 0) {
		return 0;
	}
	const QRGaloisTables &gf = _qr_gf();
	return gf.exp[(int)gf.log[p_a] + (int)gf.log[p_b]];
}

// The QR generator polynomial of the requested degree, which is by definition
// (x - a^0)(x - a^1)...(x - a^(degree-1)). Coefficients are stored highest
// degree first, so r_divisor[0] is the (always 1) leading coefficient.
static void _qr_rs_compute_divisor(int p_degree, PackedByteArray &r_divisor) {
	r_divisor.resize(p_degree + 1);
	uint8_t *div = r_divisor.ptrw();
	for (int i = 0; i <= p_degree; i++) {
		div[i] = 0;
	}
	div[0] = 1;

	const QRGaloisTables &gf = _qr_gf();
	int len = 1;
	for (int i = 0; i < p_degree; i++) {
		// Multiply the polynomial so far by (x + a^i). Walking down means each
		// slot still holds its old value when the slot above it is read.
		const uint8_t root = gf.exp[i];
		for (int j = len; j >= 1; j--) {
			div[j] ^= _qr_gf_mul(div[j - 1], root);
		}
		len++;
	}
}

// Remainder of p_data * x^degree divided by the generator polynomial: the
// error correction codewords for one block.
static void _qr_rs_remainder(const uint8_t *p_data, int p_len, const uint8_t *p_divisor,
		int p_degree, uint8_t *r_remainder) {
	for (int i = 0; i < p_degree; i++) {
		r_remainder[i] = 0;
	}
	for (int i = 0; i < p_len; i++) {
		const uint8_t factor = p_data[i] ^ r_remainder[0];
		for (int j = 0; j < p_degree - 1; j++) {
			r_remainder[j] = r_remainder[j + 1];
		}
		r_remainder[p_degree - 1] = 0;
		for (int j = 0; j < p_degree; j++) {
			r_remainder[j] ^= _qr_gf_mul(p_divisor[j + 1], factor);
		}
	}
}

/**************************************************************************/
/* Data encoding                                                          */
/**************************************************************************/

static int _qr_data_codewords(int p_version) {
	const QRVersionSpec &spec = QR_VERSION_SPECS[p_version];
	return spec.group1_blocks * spec.group1_data + spec.group2_blocks * spec.group2_data;
}

// Byte mode uses an 8-bit character count up to version 9 and a 16-bit one
// from version 10, which is why version 10 costs an extra byte of header.
static int _qr_char_count_bits(int p_version) {
	return p_version >= 10 ? 16 : 8;
}

static int _qr_choose_version(int p_len) {
	for (int version = 1; version <= QR_MAX_VERSION; version++) {
		const int needed = 4 + _qr_char_count_bits(version) + 8 * p_len;
		if (needed <= _qr_data_codewords(version) * 8) {
			return version;
		}
	}
	return -1;
}

struct QRBitWriter {
	PackedByteArray bytes;
	int bit_length = 0;

	void put_bit(uint32_t p_bit) {
		if ((bit_length & 7) == 0) {
			bytes.push_back(0);
		}
		if (p_bit) {
			bytes.write[bit_length >> 3] |= (uint8_t)(0x80 >> (bit_length & 7));
		}
		bit_length++;
	}

	void put(uint32_t p_value, int p_bits) {
		for (int i = p_bits - 1; i >= 0; i--) {
			put_bit((p_value >> i) & 1);
		}
	}
};

static void _qr_build_data_codewords(const uint8_t *p_payload, int p_len, int p_version,
		PackedByteArray &r_data) {
	const int capacity = _qr_data_codewords(p_version);

	QRBitWriter writer;
	writer.put(0x4, 4); // byte mode indicator
	writer.put((uint32_t)p_len, _qr_char_count_bits(p_version));
	for (int i = 0; i < p_len; i++) {
		writer.put(p_payload[i], 8);
	}

	// Terminator: up to four zero bits, truncated if the symbol is nearly full.
	const int terminator = MIN(4, capacity * 8 - writer.bit_length);
	for (int i = 0; i < terminator; i++) {
		writer.put_bit(0);
	}
	while ((writer.bit_length & 7) != 0) {
		writer.put_bit(0);
	}

	r_data = writer.bytes;
	const uint8_t pad[2] = { 0xEC, 0x11 };
	int pad_index = 0;
	while (r_data.size() < capacity) {
		r_data.push_back(pad[pad_index & 1]);
		pad_index++;
	}
}

// Splits the data codewords into blocks, appends each block's error correction
// codewords, and interleaves both: first the nth data codeword of every block,
// then the nth EC codeword of every block. Short blocks simply run out first.
static void _qr_interleave(const PackedByteArray &p_data, int p_version, PackedByteArray &r_out) {
	const QRVersionSpec &spec = QR_VERSION_SPECS[p_version];
	const int ec_len = spec.ec_per_block;
	const int block_count = spec.group1_blocks + spec.group2_blocks;

	PackedByteArray divisor;
	_qr_rs_compute_divisor(ec_len, divisor);

	PackedByteArray ec;
	ec.resize(block_count * ec_len);
	uint8_t *ec_words = ec.ptrw();

	const uint8_t *data = p_data.ptr();
	int offsets[QR_MAX_VERSION * 2];
	int lengths[QR_MAX_VERSION * 2];
	int offset = 0;
	int max_data = 0;
	for (int b = 0; b < block_count; b++) {
		lengths[b] = (b < spec.group1_blocks) ? spec.group1_data : spec.group2_data;
		offsets[b] = offset;
		offset += lengths[b];
		max_data = MAX(max_data, lengths[b]);
		_qr_rs_remainder(data + offsets[b], lengths[b], divisor.ptr(), ec_len, ec_words + b * ec_len);
	}

	r_out.resize(p_data.size() + block_count * ec_len);
	uint8_t *out = r_out.ptrw();
	int written = 0;
	for (int i = 0; i < max_data; i++) {
		for (int b = 0; b < block_count; b++) {
			if (i < lengths[b]) {
				out[written++] = data[offsets[b] + i];
			}
		}
	}
	for (int i = 0; i < ec_len; i++) {
		for (int b = 0; b < block_count; b++) {
			out[written++] = ec_words[b * ec_len + i];
		}
	}
}

/**************************************************************************/
/* Symbol construction                                                    */
/**************************************************************************/

struct QRSymbol {
	int size = 0;
	PackedByteArray modules; // 1 = dark
	PackedByteArray reserved; // 1 = function module, off limits to data and masking
};

static void _qr_set_function(QRSymbol &r_symbol, int p_row, int p_col, bool p_dark) {
	const int index = p_row * r_symbol.size + p_col;
	r_symbol.modules.write[index] = p_dark ? 1 : 0;
	r_symbol.reserved.write[index] = 1;
}

static void _qr_draw_function_patterns(QRSymbol &r_symbol, int p_version) {
	const int size = r_symbol.size;

	// Finder patterns. The loop covers -1..7 in both axes so that the
	// one-module separator falls out of the same darkness test.
	const int finders[3][2] = { { 0, 0 }, { 0, size - 7 }, { size - 7, 0 } };
	for (int f = 0; f < 3; f++) {
		for (int r = -1; r <= 7; r++) {
			for (int c = -1; c <= 7; c++) {
				const int row = finders[f][0] + r;
				const int col = finders[f][1] + c;
				if (row < 0 || row >= size || col < 0 || col >= size) {
					continue;
				}
				const bool dark = (r >= 0 && r <= 6 && (c == 0 || c == 6)) ||
						(c >= 0 && c <= 6 && (r == 0 || r == 6)) ||
						(r >= 2 && r <= 4 && c >= 2 && c <= 4);
				_qr_set_function(r_symbol, row, col, dark);
			}
		}
	}

	// Alignment patterns, drawn before the timing patterns on purpose: from
	// version 7 there are centres sitting on row/column 6, and those are legal
	// (the pattern's own modules agree with the timing line it crosses). Only
	// the centres already covered by a finder pattern are skipped.
	const int alignment_count = QR_ALIGNMENT_COUNT[p_version];
	for (int i = 0; i < alignment_count; i++) {
		for (int j = 0; j < alignment_count; j++) {
			const int row = QR_ALIGNMENT_POSITIONS[p_version][i];
			const int col = QR_ALIGNMENT_POSITIONS[p_version][j];
			if (r_symbol.reserved[row * size + col]) {
				continue;
			}
			for (int dr = -2; dr <= 2; dr++) {
				for (int dc = -2; dc <= 2; dc++) {
					const bool dark = dr == -2 || dr == 2 || dc == -2 || dc == 2 || (dr == 0 && dc == 0);
					_qr_set_function(r_symbol, row + dr, col + dc, dark);
				}
			}
		}
	}

	// Timing patterns: the alternating row and column that bridge the finders
	// and give a decoder its module grid.
	for (int i = 0; i < size; i++) {
		if (!r_symbol.reserved[6 * size + i]) {
			_qr_set_function(r_symbol, 6, i, (i % 2) == 0);
		}
		if (!r_symbol.reserved[i * size + 6]) {
			_qr_set_function(r_symbol, i, 6, (i % 2) == 0);
		}
	}

	// Reserve both format information areas. They stay light for now: the real
	// bits depend on the mask, which has not been chosen yet.
	for (int i = 0; i <= 8; i++) {
		if (!r_symbol.reserved[8 * size + i]) {
			_qr_set_function(r_symbol, 8, i, false);
		}
		if (!r_symbol.reserved[i * size + 8]) {
			_qr_set_function(r_symbol, i, 8, false);
		}
	}
	for (int i = 0; i < 8; i++) {
		_qr_set_function(r_symbol, 8, size - 1 - i, false);
		_qr_set_function(r_symbol, size - 1 - i, 8, false);
	}

	// Version information blocks, likewise reserved and filled in at the end.
	if (p_version >= 7) {
		for (int i = 0; i < 18; i++) {
			_qr_set_function(r_symbol, i / 3, size - 11 + i % 3, false);
			_qr_set_function(r_symbol, size - 11 + i % 3, i / 3, false);
		}
	}
}

// Walks the symbol in two-module-wide columns from the bottom-right corner,
// alternating upwards and downwards, writing one bit per free module.
static void _qr_place_codewords(QRSymbol &r_symbol, const PackedByteArray &p_codewords) {
	const int size = r_symbol.size;
	const uint8_t *codewords = p_codewords.ptr();
	const int total_bits = p_codewords.size() * 8;
	uint8_t *modules = r_symbol.modules.ptrw();
	const uint8_t *reserved = r_symbol.reserved.ptr();

	int bit = 0;
	bool upward = true;
	for (int right = size - 1; right >= 1; right -= 2) {
		// Column 6 is the vertical timing pattern and carries no data, so the
		// column pairs shift one to the left once the walk reaches it.
		if (right == 6) {
			right = 5;
		}
		for (int i = 0; i < size; i++) {
			const int row = upward ? (size - 1 - i) : i;
			for (int j = 0; j < 2; j++) {
				const int index = row * size + (right - j);
				if (reserved[index]) {
					continue;
				}
				// The symbol usually has a few free modules left over after the
				// last codeword; the standard fills them with light modules.
				uint8_t dark = 0;
				if (bit < total_bits) {
					dark = (codewords[bit >> 3] >> (7 - (bit & 7))) & 1;
				}
				modules[index] = dark;
				bit++;
			}
		}
		upward = !upward;
	}
}

static bool _qr_mask_condition(int p_mask, int p_row, int p_col) {
	switch (p_mask) {
		case 0:
			return ((p_row + p_col) % 2) == 0;
		case 1:
			return (p_row % 2) == 0;
		case 2:
			return (p_col % 3) == 0;
		case 3:
			return ((p_row + p_col) % 3) == 0;
		case 4:
			return ((p_row / 2 + p_col / 3) % 2) == 0;
		case 5:
			return (((p_row * p_col) % 2) + ((p_row * p_col) % 3)) == 0;
		case 6:
			return ((((p_row * p_col) % 2) + ((p_row * p_col) % 3)) % 2) == 0;
		default:
			return ((((p_row + p_col) % 2) + ((p_row * p_col) % 3)) % 2) == 0;
	}
}

// XOR, so calling it twice with the same mask undoes it. That is what lets the
// mask search score all eight candidates on a single symbol.
static void _qr_apply_mask(QRSymbol &r_symbol, int p_mask) {
	const int size = r_symbol.size;
	uint8_t *modules = r_symbol.modules.ptrw();
	const uint8_t *reserved = r_symbol.reserved.ptr();
	for (int row = 0; row < size; row++) {
		for (int col = 0; col < size; col++) {
			const int index = row * size + col;
			if (!reserved[index] && _qr_mask_condition(p_mask, row, col)) {
				modules[index] ^= 1;
			}
		}
	}
}

/**************************************************************************/
/* Mask evaluation                                                        */
/**************************************************************************/

// The four penalty rules from the standard, all of them proxies for "would a
// scanner have a hard time with this?":
//   N1  long single-colour runs, which look like a timing pattern gone wrong
//   N2  2x2 blocks of one colour, which blur together
//   N3  the 1:1:3:1:1 finder-like sequence, which a scanner may take for a
//       real finder pattern and mislocate the symbol
//   N4  a dark/light balance far from 50%, which costs contrast headroom
// The lowest total wins.
static int _qr_compute_penalty(const QRSymbol &p_symbol) {
	const int size = p_symbol.size;
	const uint8_t *modules = p_symbol.modules.ptr();
	int score = 0;

	// N3 looks for 1011101 flanked by four light modules on either side, as an
	// 11-module window that must fit entirely inside the symbol.
	const uint32_t finder_a = 0x5D0; // 10111010000
	const uint32_t finder_b = 0x05D; // 00001011101

	// One pass handles rule 1 and rule 3 for both a row and a column at a time.
	for (int a = 0; a < size; a++) {
		int run_row = 0;
		int run_col = 0;
		uint8_t prev_row = 0;
		uint8_t prev_col = 0;
		uint32_t window_row = 0;
		uint32_t window_col = 0;

		for (int b = 0; b < size; b++) {
			const uint8_t cur_row = modules[a * size + b];
			const uint8_t cur_col = modules[b * size + a];

			if (b > 0 && cur_row == prev_row) {
				run_row++;
			} else {
				if (run_row >= 5) {
					score += QR_PENALTY_N1 + (run_row - 5);
				}
				run_row = 1;
			}
			prev_row = cur_row;

			if (b > 0 && cur_col == prev_col) {
				run_col++;
			} else {
				if (run_col >= 5) {
					score += QR_PENALTY_N1 + (run_col - 5);
				}
				run_col = 1;
			}
			prev_col = cur_col;

			window_row = ((window_row << 1) | cur_row) & 0x7FF;
			window_col = ((window_col << 1) | cur_col) & 0x7FF;
			if (b >= 10) {
				if (window_row == finder_a || window_row == finder_b) {
					score += QR_PENALTY_N3;
				}
				if (window_col == finder_a || window_col == finder_b) {
					score += QR_PENALTY_N3;
				}
			}
		}

		if (run_row >= 5) {
			score += QR_PENALTY_N1 + (run_row - 5);
		}
		if (run_col >= 5) {
			score += QR_PENALTY_N1 + (run_col - 5);
		}
	}

	// N2: every 2x2 block of a single colour, counted once per top-left corner.
	for (int row = 0; row < size - 1; row++) {
		for (int col = 0; col < size - 1; col++) {
			const uint8_t value = modules[row * size + col];
			if (value == modules[row * size + col + 1] &&
					value == modules[(row + 1) * size + col] &&
					value == modules[(row + 1) * size + col + 1]) {
				score += QR_PENALTY_N2;
			}
		}
	}

	// N4: ten points for every full 5% the dark ratio strays from 50%. Kept in
	// integers: |dark/total - 1/2| / (1/20) == |dark*100 - total*50| / (total*5).
	int dark = 0;
	const int total = size * size;
	for (int i = 0; i < total; i++) {
		dark += modules[i];
	}
	score += (Math::abs(dark * 100 - total * 50) / (total * 5)) * QR_PENALTY_N4;

	return score;
}

/**************************************************************************/
/* Format and version information                                         */
/**************************************************************************/

// BCH(15,5) over the five format data bits, then the standard XOR mask so that
// an all-zero format never produces an all-light run.
static uint32_t _qr_format_bits(int p_mask) {
	const uint32_t data = (uint32_t)p_mask; // level M is 0b00, so only the mask remains
	uint32_t rem = data << 10;
	for (int i = 4; i >= 0; i--) {
		if (rem & (1u << (10 + i))) {
			rem ^= 0x537u << i;
		}
	}
	return ((data << 10) | rem) ^ 0x5412u;
}

// BCH(18,6) over the version number, unmasked.
static uint32_t _qr_version_bits(int p_version) {
	const uint32_t data = (uint32_t)p_version;
	uint32_t rem = data << 12;
	for (int i = 5; i >= 0; i--) {
		if (rem & (1u << (12 + i))) {
			rem ^= 0x1F25u << i;
		}
	}
	return (data << 12) | rem;
}

// The fifteen format bits appear twice: once wrapped around the top-left
// finder, once split between the bottom-left and top-right corners. Written
// LSB first, which is why the index ranges below look arbitrary.
static void _qr_draw_format_bits(QRSymbol &r_symbol, int p_mask) {
	const int size = r_symbol.size;
	uint8_t *modules = r_symbol.modules.ptrw();
	const uint32_t bits = _qr_format_bits(p_mask);

	for (int i = 0; i < 15; i++) {
		const uint8_t bit = (uint8_t)((bits >> i) & 1);

		// Column 8: down the left of the symbol, then the bottom-left run.
		if (i < 6) {
			modules[i * size + 8] = bit;
		} else if (i < 8) {
			modules[(i + 1) * size + 8] = bit; // steps over the timing row
		} else {
			modules[(size - 15 + i) * size + 8] = bit;
		}

		// Row 8: the top-right run, then leftwards past the top-left finder.
		if (i < 8) {
			modules[8 * size + (size - 1 - i)] = bit;
		} else if (i == 8) {
			modules[8 * size + 7] = bit;
		} else {
			modules[8 * size + (14 - i)] = bit; // steps over the timing column
		}
	}

	// The dark module: always set, and not part of the format data.
	modules[(size - 8) * size + 8] = 1;
}

// Two 3x6 blocks, one left of the top-right finder and one above the
// bottom-left finder, transposed copies of each other.
static void _qr_draw_version_bits(QRSymbol &r_symbol, int p_version) {
	if (p_version < 7) {
		return;
	}
	const int size = r_symbol.size;
	uint8_t *modules = r_symbol.modules.ptrw();
	const uint32_t bits = _qr_version_bits(p_version);

	for (int i = 0; i < 18; i++) {
		const uint8_t bit = (uint8_t)((bits >> i) & 1);
		modules[(i / 3) * size + (size - 11 + i % 3)] = bit;
		modules[(size - 11 + i % 3) * size + (i / 3)] = bit;
	}
}

/**************************************************************************/
/* Entry point                                                            */
/**************************************************************************/

bool AIRemoteQR::encode(const String &p_text, int &r_size, PackedByteArray &r_modules) {
	const CharString utf8 = p_text.utf8();
	const int length = utf8.length();
	if (length > MAX_BYTES) {
		return false;
	}

	const int version = _qr_choose_version(length);
	if (version < 0) {
		return false;
	}

	const int size = version * 4 + 17;
	QRSymbol symbol;
	symbol.size = size;
	symbol.modules.resize(size * size);
	symbol.reserved.resize(size * size);
	{
		uint8_t *modules = symbol.modules.ptrw();
		uint8_t *reserved = symbol.reserved.ptrw();
		for (int i = 0; i < size * size; i++) {
			modules[i] = 0;
			reserved[i] = 0;
		}
	}

	_qr_draw_function_patterns(symbol, version);

	PackedByteArray data;
	_qr_build_data_codewords((const uint8_t *)utf8.get_data(), length, version, data);
	PackedByteArray codewords;
	_qr_interleave(data, version, codewords);
	_qr_place_codewords(symbol, codewords);

	// Score all eight masks and keep the cheapest. The format and version
	// modules are still light at this point, so they score as light for every
	// candidate — they carry no data of their own to bias the comparison.
	int best_mask = 0;
	int best_penalty = 0;
	for (int mask = 0; mask < 8; mask++) {
		_qr_apply_mask(symbol, mask);
		const int penalty = _qr_compute_penalty(symbol);
		_qr_apply_mask(symbol, mask);
		if (mask == 0 || penalty < best_penalty) {
			best_penalty = penalty;
			best_mask = mask;
		}
	}

	_qr_apply_mask(symbol, best_mask);
	_qr_draw_format_bits(symbol, best_mask);
	_qr_draw_version_bits(symbol, version);

	r_size = size;
	r_modules = symbol.modules;
	return true;
}
