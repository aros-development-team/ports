use super::*;
use egui::{pos2, Color32};

fn vertex(x: f32, y: f32, u: f32, v: f32, color: Color32) -> Vertex {
    Vertex { pos: pos2(x, y), uv: pos2(u, v), color }
}

fn draw(vertices: &[Vertex; 3], texture: Option<&Texture>) -> Vec<u8> {
    let mut frame = vec![0; 8 * 8 * 4];
    for px in frame.chunks_exact_mut(4) { px.copy_from_slice(&[10, 20, 30, 255]); }
    triangle(&mut frame, 8, [1, 1, 7, 7], 1.0, texture, &vertices[0], &vertices[1], &vertices[2]);
    frame
}

#[test]
fn uniform_uv_samples_once() {
    let t = Texture { width: 2, height: 2, pixels: vec![[255; 4]; 4], linear: true };
    let v = [vertex(0., 0., 0.5, 0.5, Color32::WHITE), vertex(8., 0., 0.5, 0.5, Color32::WHITE), vertex(0., 8., 0.5, 0.5, Color32::WHITE)];
    SAMPLE_COUNT.with(|n| n.set(0));
    let out = draw(&v, Some(&t));
    assert!(out.chunks_exact(4).any(|p| p == [255, 255, 255, 255]));
    SAMPLE_COUNT.with(|n| assert_eq!(n.get(), 1));
}

#[test]
fn varying_uv_still_samples_per_pixel() {
    let t = Texture { width: 2, height: 2, pixels: vec![[255; 4]; 4], linear: true };
    let v = [vertex(0., 0., 0., 0., Color32::WHITE), vertex(8., 0., 1., 0., Color32::WHITE), vertex(0., 8., 0., 1., Color32::WHITE)];
    SAMPLE_COUNT.with(|n| n.set(0));
    let _ = draw(&v, Some(&t));
    SAMPLE_COUNT.with(|n| assert!(n.get() > 1));
}

#[test]
fn white_constant_sample_matches_untextured_color_and_alpha() {
    for linear in [false, true] {
        let t = Texture { width: 2, height: 2, pixels: vec![[255; 4]; 4], linear };
        for color in [Color32::WHITE, Color32::TRANSPARENT, Color32::from_rgba_premultiplied(40, 20, 10, 128)] {
            for gradient in [false, true] {
                let other = if gradient { Color32::from_rgba_premultiplied(30, 70, 90, 150) } else { color };
                let v = [vertex(0., 0., 0.5, 0.5, color), vertex(8., 0., 0.5, 0.5, other), vertex(0., 8., 0.5, 0.5, color)];
                assert_eq!(draw(&v, Some(&t)), draw(&v, None));
            }
        }
    }
}

#[test]
fn constant_colored_sample_preserves_premultiplied_alpha_and_winding() {
    let texel = [40, 20, 10, 128];
    let color = Color32::from_rgba_premultiplied(texel[0], texel[1], texel[2], texel[3]);
    for linear in [false, true] {
        let texture = Texture { width: 2, height: 2, pixels: vec![texel; 4], linear };
        let vertices = [vertex(0., 0., 0.5, 0.5, Color32::WHITE), vertex(8., 0., 0.5, 0.5, Color32::WHITE), vertex(0., 8., 0.5, 0.5, Color32::WHITE)];
        let plain = vertices.map(|v| Vertex { color, ..v });
        let expected = draw(&plain, None);
        assert_eq!(draw(&vertices, Some(&texture)), expected);
        let reversed = [vertices[0], vertices[2], vertices[1]];
        assert_eq!(draw(&reversed, Some(&texture)), expected);
        assert_eq!(&expected[0..4], &[10, 20, 30, 255]);
    }
}
