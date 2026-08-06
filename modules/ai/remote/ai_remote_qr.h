/**************************************************************************/
/*  ai_remote_qr.h                                                        */
/**************************************************************************/
/* Self-contained QR Code encoder: byte mode, error correction level M,   */
/* versions 1-10. Produces a bare module matrix, no image and no I/O.     */
/*                                                                        */
/* Hand-rolled because the pairing flow needs a scannable code in the      */
/* editor and on the web client, and pulling a QR library into a module    */
/* that already has to build with a restricted SCons env is not worth it   */
/* for the handful of tables involved.                                     */
/**************************************************************************/

#ifndef AI_REMOTE_QR_H
#define AI_REMOTE_QR_H

#include "core/string/ustring.h"
#include "core/variant/variant.h"

class AIRemoteQR {
public:
	// Encodes p_text as a QR Code (byte mode, error correction level M).
	// On success r_size is the side length in modules and r_modules is
	// r_size*r_size bytes, row-major, 1 = dark, 0 = light. The quiet zone is
	// NOT included. Returns false if the text does not fit (see MAX_BYTES).
	static bool encode(const String &p_text, int &r_size, PackedByteArray &r_modules);

	// Largest UTF-8 payload encode() accepts, in bytes. Version 10 at level M
	// carries 216 data codewords, but the mode indicator and the 16-bit
	// character count consume three of them. Anything longer is rejected
	// rather than silently truncated.
	static const int MAX_BYTES = 213;
};

#endif // AI_REMOTE_QR_H
