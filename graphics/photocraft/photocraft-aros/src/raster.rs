//! A software renderer for egui's tessellated output: textured, vertex-coloured triangles with
//! premultiplied alpha, drawn into an RGBA frame buffer that the window blits. No GPU is needed
//! (AROS has no wgpu backend), and egui's own feathering does the anti-aliasing.

use std::collections::HashMap;

use egui::epaint::textures::{TextureFilter, TexturesDelta};
use egui::epaint::{ClippedPrimitive, ImageData, ImageDelta, Primitive, TextureId, Vertex};

struct Texture {
    width: usize,
    height: usize,
    /// Premultiplied sRGBA, row by row.
    pixels: Vec<[u8; 4]>,
    linear: bool,
}

impl Texture {
    #[inline]
    fn texel(&self, x: i32, y: i32) -> [u8; 4] {
        let x = x.clamp(0, self.width as i32 - 1) as usize;
        let y = y.clamp(0, self.height as i32 - 1) as usize;
        self.pixels.get(y * self.width + x).copied().unwrap_or([0; 4])
    }

    /// Samples at normalized `(u, v)`: bilinear for textures asking for it, else nearest.
    #[inline]
    fn sample(&self, u: f32, v: f32) -> [f32; 4] {
        let fx = u * self.width as f32 - 0.5;
        let fy = v * self.height as f32 - 0.5;
        if !self.linear {
            let t = self.texel((fx + 0.5).floor() as i32, (fy + 0.5).floor() as i32);
            return [t[0] as f32, t[1] as f32, t[2] as f32, t[3] as f32];
        }
        let x0 = fx.floor();
        let y0 = fy.floor();
        let tx = fx - x0;
        let ty = fy - y0;
        let (x0, y0) = (x0 as i32, y0 as i32);
        let a = self.texel(x0, y0);
        let b = self.texel(x0 + 1, y0);
        let c = self.texel(x0, y0 + 1);
        let d = self.texel(x0 + 1, y0 + 1);
        let mut out = [0.0f32; 4];
        for i in 0..4 {
            let top = a[i] as f32 * (1.0 - tx) + b[i] as f32 * tx;
            let bottom = c[i] as f32 * (1.0 - tx) + d[i] as f32 * tx;
            out[i] = top * (1.0 - ty) + bottom * ty;
        }
        out
    }
}

pub struct Renderer {
    textures: HashMap<TextureId, Texture>,
    /// RGBA, `width * height * 4` bytes, opaque after `clear`.
    pub frame: Vec<u8>,
    pub width: usize,
    pub height: usize,
}

impl Renderer {
    pub fn new(width: usize, height: usize) -> Self {
        let mut r = Self { textures: HashMap::new(), frame: Vec::new(), width: 0, height: 0 };
        r.resize(width, height);
        r
    }

    pub fn resize(&mut self, width: usize, height: usize) {
        let (width, height) = (width.max(1), height.max(1));
        if (width, height) != (self.width, self.height) {
            self.width = width;
            self.height = height;
            self.frame = vec![0; width * height * 4];
        }
    }

    pub fn stride(&self) -> usize {
        self.width * 4
    }

    pub fn clear(&mut self, rgb: [u8; 3]) {
        for px in self.frame.chunks_exact_mut(4) {
            px.copy_from_slice(&[rgb[0], rgb[1], rgb[2], 255]);
        }
    }

    /// Applies the frame's texture uploads (before painting).
    pub fn update_textures(&mut self, delta: &TexturesDelta) {
        for (id, deltas) in &delta.set {
            for d in deltas {
                self.set_texture(*id, d);
            }
        }
    }

    /// Frees the frame's released textures (after painting).
    pub fn free_textures(&mut self, delta: &TexturesDelta) {
        for id in &delta.free {
            self.textures.remove(id);
        }
    }

    fn set_texture(&mut self, id: TextureId, delta: &ImageDelta) {
        let ImageData::Color(image) = &delta.image;
        let [w, h] = image.size;
        let linear = delta.options.magnification == TextureFilter::Linear;
        match delta.pos {
            None => {
                self.textures.insert(id, Texture { width: w, height: h, pixels: image.pixels.iter().map(|c| c.to_array()).collect(), linear });
            }
            Some([x, y]) => {
                let Some(t) = self.textures.get_mut(&id) else { return };
                for row in 0..h {
                    let ty = y + row;
                    if ty >= t.height {
                        break;
                    }
                    for col in 0..w {
                        let tx = x + col;
                        if tx >= t.width {
                            break;
                        }
                        if let (Some(dst), Some(src)) = (t.pixels.get_mut(ty * t.width + tx), image.pixels.get(row * w + col)) {
                            *dst = src.to_array();
                        }
                    }
                }
            }
        }
    }

    pub fn paint(&mut self, primitives: &[ClippedPrimitive], pixels_per_point: f32) {
        for p in primitives {
            let Primitive::Mesh(mesh) = &p.primitive else { continue };
            let clip = p.clip_rect;
            let x0 = ((clip.min.x * pixels_per_point).floor() as i32).max(0);
            let y0 = ((clip.min.y * pixels_per_point).floor() as i32).max(0);
            let x1 = ((clip.max.x * pixels_per_point).ceil() as i32).min(self.width as i32);
            let y1 = ((clip.max.y * pixels_per_point).ceil() as i32).min(self.height as i32);
            if x1 <= x0 || y1 <= y0 {
                continue;
            }
            let texture = self.textures.get(&mesh.texture_id);
            for tri in mesh.indices.chunks_exact(3) {
                let (Some(a), Some(b), Some(c)) =
                    (mesh.vertices.get(tri[0] as usize), mesh.vertices.get(tri[1] as usize), mesh.vertices.get(tri[2] as usize))
                else {
                    continue;
                };
                triangle(&mut self.frame, self.width, [x0, y0, x1, y1], pixels_per_point, texture, a, b, c);
            }
        }
    }
}

/// Rasterizes one triangle into `frame` within the clip box (pixel units, max exclusive).
#[allow(clippy::too_many_arguments)]
fn triangle(frame: &mut [u8], width: usize, clip: [i32; 4], ppp: f32, texture: Option<&Texture>, a: &Vertex, b: &Vertex, c: &Vertex) {
    let (ax, ay) = (a.pos.x * ppp, a.pos.y * ppp);
    let (bx, by) = (b.pos.x * ppp, b.pos.y * ppp);
    let (cx, cy) = (c.pos.x * ppp, c.pos.y * ppp);
    let area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if area.abs() < 1e-6 || !area.is_finite() {
        return;
    }
    let inv_area = 1.0 / area;
    let min_x = (ax.min(bx).min(cx).floor() as i32).max(clip[0]);
    let max_x = (ax.max(bx).max(cx).ceil() as i32).min(clip[2]);
    let min_y = (ay.min(by).min(cy).floor() as i32).max(clip[1]);
    let max_y = (ay.max(by).max(cy).ceil() as i32).min(clip[3]);
    if min_x >= max_x || min_y >= max_y {
        return;
    }
    let ca = a.color.to_array();
    let cb = b.color.to_array();
    let cc = c.color.to_array();
    let flat_color = ca == cb && cb == cc;
    // How each edge function changes per pixel step to the right (d/dx of the expressions below).
    let e0_dx = by - cy;
    let e1_dx = cy - ay;
    let e2_dx = ay - by;
    for py in min_y..max_y {
        let sy = py as f32 + 0.5;
        let sx0 = min_x as f32 + 0.5;
        let mut w0 = (bx - sx0) * (cy - sy) - (by - sy) * (cx - sx0);
        let mut w1 = (cx - sx0) * (ay - sy) - (cy - sy) * (ax - sx0);
        let mut w2 = (ax - sx0) * (by - sy) - (ay - sy) * (bx - sx0);
        // Normalized to the triangle's winding: inside when all three are >= 0.
        let sign = if area > 0.0 { 1.0 } else { -1.0 };
        let row = py as usize * width;
        for px in min_x..max_x {
            if w0 * sign >= 0.0 && w1 * sign >= 0.0 && w2 * sign >= 0.0 {
                let l0 = w0 * inv_area;
                let l1 = w1 * inv_area;
                let l2 = w2 * inv_area;
                let color = if flat_color {
                    [ca[0] as f32, ca[1] as f32, ca[2] as f32, ca[3] as f32]
                } else {
                    let mut v = [0.0f32; 4];
                    for i in 0..4 {
                        v[i] = ca[i] as f32 * l0 + cb[i] as f32 * l1 + cc[i] as f32 * l2;
                    }
                    v
                };
                let src = match texture {
                    Some(t) => {
                        let u = a.uv.x * l0 + b.uv.x * l1 + c.uv.x * l2;
                        let v = a.uv.y * l0 + b.uv.y * l1 + c.uv.y * l2;
                        let s = t.sample(u, v);
                        [color[0] * s[0] / 255.0, color[1] * s[1] / 255.0, color[2] * s[2] / 255.0, color[3] * s[3] / 255.0]
                    }
                    None => color,
                };
                let alpha = src[3];
                if alpha > 0.0 {
                    let i = (row + px as usize) * 4;
                    if let Some(dst) = frame.get_mut(i..i + 4) {
                        if alpha >= 254.5 {
                            dst[0] = src[0] as u8;
                            dst[1] = src[1] as u8;
                            dst[2] = src[2] as u8;
                            dst[3] = 255;
                        } else {
                            let keep = (255.0 - alpha) / 255.0;
                            dst[0] = (src[0] + dst[0] as f32 * keep).min(255.0) as u8;
                            dst[1] = (src[1] + dst[1] as f32 * keep).min(255.0) as u8;
                            dst[2] = (src[2] + dst[2] as f32 * keep).min(255.0) as u8;
                            dst[3] = (alpha + dst[3] as f32 * keep).min(255.0) as u8;
                        }
                    }
                }
            }
            w0 += e0_dx;
            w1 += e1_dx;
            w2 += e2_dx;
        }
    }
}
