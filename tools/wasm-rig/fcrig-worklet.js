// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The AudioWorklet processor of fcrig: one device of a .orbitrig pack, playing (tools/fc_rig_abi.h over
// felitronics::rigplayer) on the audio thread. Registered as 'fc-rig'. The processor itself — its messages, its start,
// its loads — is the rig half every pack player's worklet shares, in ./rig-port.js (where its whole contract is
// written); this file is fcrig's: its module's glue, and the registration. The two files travel together, beside
// fcrig.mjs and fcrig.wasm, under these names (README.md).

import createFcRig from './fcrig.mjs';
import { RigProcessor } from './rig-port.js';

class FcRigProcessor extends RigProcessor {
    constructor(options) {
        super(options, createFcRig);
    }
}

registerProcessor('fc-rig', FcRigProcessor);
