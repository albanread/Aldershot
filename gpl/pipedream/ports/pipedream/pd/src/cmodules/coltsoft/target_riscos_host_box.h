/* target_riscos_host_box.h */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Copyright (C) 2026 the PipeDream box port */

/* The box's target header: Stuart's clang one (the SAL neutralisation
 * and the Norcroft-workaround environment), with one change -- the
 * box's staged CLib has a C99 <complex.h>, and clang's double _Complex
 * is native, so PipeDream's own struct-COMPLEX implementation is not
 * wanted. */

#include "target_riscos_host_clang.h"

#undef USE_OWN_COMPLEX_IMPL

/* end of target_riscos_host_box.h */
