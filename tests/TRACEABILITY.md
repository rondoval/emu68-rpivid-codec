# rpivid.codec: traceability

Written by the suite's host tier (`run.sh host` rewrites it): every item the suite must cover, with the tests that cover it.

**Uncovered: 0** of 25 items; known gaps: 1.

## The backend's entry points

| Item | Tests |
|---|---|
| VCB_GetInfo | `test_main10.py::test_narrowed_counted` |
| VCB_QueryCodec | `test_h265.py::test_codec_row` |
| VCB_OpenSession | `test_h265.py::test_bit_exact` |
| VCB_CloseSession | `test_h265.py::test_bit_exact` |
| VCB_ImportPacket | `test_h265.py::test_imported_packets` |
| VCB_ImportFrame | `test_h265.py::test_imported_frames`, `test_h265.py::test_imported_frames_of_another_shape` |
| VCB_SendPacket | `test_h265.py::test_bit_exact` |
| VCB_SendFrame | `test_h265.py::test_backend_refusals` |
| VCB_Poll | `test_h265.py::test_bit_exact` |
| VCB_ReleaseFrame | `test_h265.py::test_bit_exact` |
| VCB_ReleasePacket | `test_h265.py::test_backend_refusals` |
| VCB_Flush | `test_h265.py::test_flush_replays` |
| VCB_SetAttrs | `test_h265.py::test_resignal_through_isp` |
| VCB_Image | `test_h265.py::test_backend_refusals` |
| VCB_Idle | `test_h265.py::test_the_block_goes_down_after_the_last_close` |
| VCB_OutputFrame | `test_h265.py::test_through_isp` |

## Events

| Item | Tests |
|---|---|
| VCEV_EndOfStream | `test_h265.py::test_end_of_stream` |
| VCEV_Error | `test_h265.py::test_refused_4k_main10` |
| VCEV_StreamInfo | `test_robust.py::test_shape_change` |

## Errors

| Item | Tests |
|---|---|
| VCERR_BADPARAM | `test_h265.py::test_backend_refusals`, `test_h265.py::test_imported_frames_of_another_shape` |
| VCERR_BADSTATE | `test_h265.py::test_backend_refusals` |
| VCERR_NOMEM | `test_robust.py::test_low_memory` |
| VCERR_STREAM | `test_robust.py::test_a_bad_sps_as_extradata`, `test_robust.py::test_a_bad_sps_in_the_stream` |
| VCERR_TRANSPORT | **known gap:** needs a block that does not answer: only fault injection in the driver makes one (the user's call: none) |
| VCERR_UNSUPPORTED | `test_conformance.py::test_refused`, `test_h265.py::test_refused_4k_main10` |
