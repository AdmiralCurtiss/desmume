#pragma once

#include "types.h"

extern u32 fakeMatrixMod[16];
extern int fakeMatrixModIdx;
extern int fakeMatrixModStep;

struct DualStrikeMatrixParameters {
	enum DualStrikeMatrixType {
		DUAL_STRIKE_MATRIX_KEEP_ORIGINAL,
		DUAL_STRIKE_MATRIX_ORTHOGRAPHIC,
		DUAL_STRIKE_MATRIX_PERSPECTIVE,
	};

	DualStrikeMatrixType Type;


	DualStrikeMatrixParameters() : Type( DUAL_STRIKE_MATRIX_KEEP_ORIGINAL )
	{}
};

extern DualStrikeMatrixParameters DualStrikeMatrixParams;
