//! H.264 on CPU: system libopenh264 via dlopen.
//!
//! The library is not bundled (H.264 patents: Cisco pays for its own builds, and
//! distros ship it as a separate package: `openh264` in Arch, `openh264` in
//! nixpkgs). If it's missing, we return a clear error and the plugin keeps running.
//!
//! Timestamps go through openh264 itself (uiInBsTimeStamp → uiOutYuvTimeStamp):
//! with B-frames it outputs pictures in display order, and pts travels with the frame.
//! The crate's wrapper can't do this, so we decode via its raw API.

use std::ptr::null_mut;
use std::time::Duration;

use openh264::OpenH264API;
use openh264::decoder::{Decoder as Oh264, DecoderConfig};
use openh264_sys2::{
    DECODER_OPTION, DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, SBufferInfo,
};

use crate::frame::FrameData;
use crate::media::video::pipeline::{DecodedFrame, Decoder, Packet, PipeResult};
use crate::media::video::yuv::{self, Chroma, ChromaPlanes, Matrix, Plane, YuvImage};

/// Where to look for the library: by soname first (ld.so finds it in /usr/lib,
/// /run/opengl-driver, the NixOS LD_LIBRARY_PATH), then by full paths.
const CANDIDATES: &[&str] = &[
    "libopenh264.so.8",
    "libopenh264.so.7",
    "libopenh264.so",
    "/usr/lib/libopenh264.so",
    "/usr/lib64/libopenh264.so",
    "/usr/lib/x86_64-linux-gnu/libopenh264.so",
];

/// Environment variable for an explicit path (e.g. a downloaded Cisco build).
const ENV_PATH: &str = "NOSHARE_COVER_OPENH264";

pub struct H264Decoder {
    dec: Oh264,
    /// Packets in a row that openh264 refused without producing a picture.
    refused: u32,
    last_state: i32,
}

/// After this many refused packets in a row, report the stream as undecodable
/// instead of silently showing nothing.
const MAX_REFUSED: u32 = 90;
/// DECODING_STATE bits worth naming in the error.
const DS_NO_PARAM_SETS: i32 = 0x10;
const DS_BITSTREAM_ERROR: i32 = 0x04;

fn load_api() -> Result<OpenH264API, String> {
    let mut tried = Vec::new();
    let explicit = std::env::var(ENV_PATH).ok();
    // NSC_LIB_OPENH264: absolute path baked in at build time (Nix: library from the store;
    // on NixOS dlopen by soname won't find it)
    for name in explicit
        .iter()
        .map(String::as_str)
        .chain(option_env!("NSC_LIB_OPENH264"))
        .chain(CANDIDATES.iter().copied())
    {
        // SAFETY: openh264 keeps a stable decoder C ABI across 2.x (so.6–so.8);
        // the crate's hash check only accepts Cisco builds; distro builds
        // come from the same sources.
        match unsafe { OpenH264API::from_blob_path_unchecked(name) } {
            Ok(api) => return Ok(api),
            Err(e) => tried.push(format!("{name}: {e}")),
        }
    }
    Err(format!(
        "libopenh264 not found (install the openh264 package or set {ENV_PATH}); tried: {}",
        tried.join("; ")
    ))
}

impl H264Decoder {
    pub fn new() -> PipeResult<Self> {
        let api = load_api()?;
        let dec = Oh264::with_api_config(api, DecoderConfig::new())
            .map_err(|e| format!("openh264: {e}"))?;
        Ok(Self {
            dec,
            refused: 0,
            last_state: 0,
        })
    }

    fn remaining(&mut self) -> usize {
        let mut n: DECODER_OPTION = 0;
        // SAFETY: GetOption writes one integer through the pointer.
        let r = unsafe {
            self.dec.raw_api().get_option(
                DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER,
                (&raw mut n).cast(),
            )
        };
        if r == 0 {
            usize::try_from(n).unwrap_or(0)
        } else {
            0
        }
    }
}

/// Frame from the openh264 buffer. The planes live until the next decoder call,
/// so convert to BGRA right away.
fn convert(dst: &[*mut u8; 3], info: &SBufferInfo) -> Option<DecodedFrame> {
    if info.iBufferStatus != 1 || dst.iter().any(|p| p.is_null()) {
        return None;
    }
    // SAFETY: when iBufferStatus == 1, the union holds sSystemBuffer.
    let sb = unsafe { info.UsrData.sSystemBuffer };
    let (w, h) = (
        usize::try_from(sb.iWidth).ok()?,
        usize::try_from(sb.iHeight).ok()?,
    );
    let (ys, cs) = (
        usize::try_from(sb.iStride[0]).ok()?,
        usize::try_from(sb.iStride[1]).ok()?,
    );
    if w == 0 || h == 0 {
        return None;
    }
    let (cw, ch) = (w.div_ceil(2), h.div_ceil(2));
    // SAFETY: openh264 guarantees stride*rows bytes per plane (I420).
    let plane = |p: *mut u8, stride: usize, width: usize, rows: usize| Plane {
        data: unsafe { std::slice::from_raw_parts(p, stride * (rows - 1) + width) },
        stride,
    };
    // openh264 outputs only 8-bit 4:2:0 and doesn't expose the VUI matrix.
    let yuv = YuvImage {
        width: u32::try_from(w).ok()?,
        height: u32::try_from(h).ok()?,
        bit_depth: 8,
        chroma: Chroma::Sub420,
        matrix: Matrix::guess(u32::try_from(h).ok()?),
        full_range: false,
        y: plane(dst[0], ys, w, h),
        uv: ChromaPlanes::Planar {
            u: plane(dst[1], cs, cw, ch),
            v: plane(dst[2], cs, cw, ch),
        },
    };
    Some(DecodedFrame {
        pts: Duration::from_micros(info.uiOutYuvTimeStamp),
        data: FrameData::Cpu(yuv::to_bgra(&yuv)?),
    })
}

fn refusal_text(state: i32) -> String {
    let why = if state & DS_NO_PARAM_SETS != 0 {
        "it rejected the SPS/PPS (unsupported profile, e.g. High 10 / 4:2:2)"
    } else if state & DS_BITSTREAM_ERROR != 0 {
        "bitstream error"
    } else {
        "no pictures"
    };
    format!("openh264 can't decode this video: {why}, state {state:#x}; try backend = \"gpu\"")
}

/// Highest level openh264 accepts (5.2). An SPS above it is rejected outright, the
/// decoder then has no parameter sets and returns no frames at all.
const OPENH264_MAX_LEVEL: u8 = 52;

/// Copy of an Annex-B packet with every SPS `level_idc` above 5.2 lowered to 5.2, or
/// `None` if nothing needs changing. Encoders sometimes write level 6.x for small videos;
/// the level only bounds buffer sizes, so clamping is harmless for streams that really
/// fit into 5.2 (anything up to 4K30).
fn clamp_sps_level(data: &[u8]) -> Option<Vec<u8>> {
    let mut out: Option<Vec<u8>> = None;
    let mut i = 0;
    while i + 3 < data.len() {
        // start code 00 00 01 (also the tail of 00 00 00 01)
        if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 {
            let nal = i + 3;
            // SPS: header (type 7), profile_idc, constraint flags, level_idc
            if nal + 3 < data.len() && data[nal] & 0x1f == 7 && data[nal + 3] > OPENH264_MAX_LEVEL {
                out.get_or_insert_with(|| data.to_vec())[nal + 3] = OPENH264_MAX_LEVEL;
            }
            i = nal;
        } else {
            i += 1;
        }
    }
    out
}

impl Decoder for H264Decoder {
    fn name(&self) -> &'static str {
        "openh264 (CPU)"
    }

    fn decode(&mut self, packet: &Packet) -> PipeResult<Vec<DecodedFrame>> {
        // Only keyframes carry SPS; copy just those when their level needs clamping.
        let patched = if packet.keyframe {
            clamp_sps_level(&packet.data)
        } else {
            None
        };
        let data: &[u8] = patched.as_deref().unwrap_or(&packet.data);
        let Ok(len) = i32::try_from(data.len()) else {
            return Ok(Vec::new());
        };
        let mut dst = [null_mut::<u8>(); 3];
        let mut info = SBufferInfo {
            uiInBsTimeStamp: u64::try_from(packet.pts.as_micros()).unwrap_or(u64::MAX),
            ..Default::default()
        };
        // SAFETY: the packet outlives the call; dst/info are our output buffers.
        // The return code is ignored: a broken frame just yields no picture, and
        // the next keyframe fixes everything.
        unsafe {
            self.last_state = self.dec.raw_api().decode_frame_no_delay(
                data.as_ptr(),
                len,
                dst.as_mut_ptr(),
                &raw mut info,
            ) as i32;
        }
        let frames: Vec<DecodedFrame> = convert(&dst, &info).into_iter().collect();
        if frames.is_empty() && self.last_state != 0 {
            self.refused += 1;
            if self.refused >= MAX_REFUSED {
                return Err(refusal_text(self.last_state));
            }
        } else {
            self.refused = 0;
        }
        Ok(frames)
    }

    fn flush(&mut self) -> PipeResult<Vec<DecodedFrame>> {
        let mut out = Vec::new();
        for _ in 0..self.remaining() {
            let mut dst = [null_mut::<u8>(); 3];
            let mut info = SBufferInfo::default();
            // SAFETY: same as in decode.
            unsafe {
                self.dec
                    .raw_api()
                    .flush_frame(dst.as_mut_ptr(), &raw mut info)
            };
            out.extend(convert(&dst, &info));
        }
        Ok(out)
    }

    fn reset(&mut self) {
        // After a loop the demuxer starts at a keyframe with SPS/PPS, and the decoder
        // reconfigures itself; the tail of the previous pass is simply dropped.
        let _ = self.flush();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sps_level_is_clamped_only_when_needed() {
        // SPS High, level 6.2 (0x3e), then an IDR slice
        let pkt = [
            0, 0, 0, 1, 0x67, 0x64, 0x00, 0x3e, 0xac, 0, 0, 1, 0x65, 0x88,
        ];
        let fixed = clamp_sps_level(&pkt).unwrap();
        assert_eq!(fixed[7], 52);
        assert_eq!(&fixed[..7], &pkt[..7]);
        assert_eq!(&fixed[8..], &pkt[8..]);
        // level 4.0 stays, nothing is copied
        let ok = [0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28, 0xac];
        assert!(clamp_sps_level(&ok).is_none());
        // truncated data doesn't panic
        assert!(clamp_sps_level(&[0, 0, 1, 0x67]).is_none());
    }
}
