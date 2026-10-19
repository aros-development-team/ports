//! egui drawn with wgpu into the window: a surface on the Intuition window (wgpu's OpenGL
//! backend renders through gl.library's GLA contexts), egui-wgpu's renderer with the depth and
//! stencil buffer the viewport's pipelines expect, and a frame's upload, render pass and
//! presentation (what egui-wgpu's winit painter does, without winit).

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use egui::epaint::ClippedPrimitive;
use egui::epaint::textures::TexturesDelta;
use egui_wgpu::{RenderState, RendererOptions, ScreenDescriptor, WgpuConfiguration, WgpuSetup};
use raw_window_handle::{ArosDisplayHandle, ArosWindowHandle, RawDisplayHandle, RawWindowHandle};

use crate::window::Window;

/// The section cap marks where it shows in the stencil (the desktop app's `STENCIL_BITS`).
pub const STENCIL_BITS: u8 = 8;

pub struct Painter {
    // Dropped in this order: the depth texture and the surface before the render state's
    // device, all before the window the surface was made on (the caller keeps the window).
    depth_view: Option<wgpu::TextureView>,
    surface: wgpu::Surface<'static>,
    rs: RenderState,
    width: u32,
    height: u32,
    depth_bits: u8,
    depth_format: Option<wgpu::TextureFormat>,
}

impl Painter {
    /// Opens wgpu's OpenGL backend on `window` and sets the renderer up. The depth buffer is a
    /// 32-bit float one with a stencil where the adapter has it (the viewport then draws with
    /// reversed depth), else 24 bits with a stencil.
    pub fn new(window: &Window) -> Result<Self, String> {
        let mut desc = wgpu::InstanceDescriptor::new_without_display_handle();
        desc.backends = wgpu::Backends::GL;
        desc.flags = wgpu::InstanceFlags::from_build_config().with_env();
        let instance = wgpu::Instance::new(desc);
        let raw = NonNull::new(window.handle() as *mut c_void).ok_or("no window")?;
        let target = wgpu::SurfaceTargetUnsafe::RawHandle {
            raw_display_handle: Some(RawDisplayHandle::Aros(ArosDisplayHandle::new())),
            raw_window_handle: RawWindowHandle::Aros(ArosWindowHandle::new(raw)),
        };
        // SAFETY: the Intuition window outlives the surface (the caller drops the painter first).
        let surface = unsafe { instance.create_surface_unsafe(target) }.map_err(|e| format!("cannot create the window's surface: {e}"))?;

        // The adapter's features decide the depth format before the render state is made.
        let adapters = pollster::block_on(instance.enumerate_adapters(wgpu::Backends::GL));
        let float_depth = !adapters.is_empty() && adapters.iter().all(|a| a.features().contains(wgpu::Features::DEPTH32FLOAT_STENCIL8));
        let depth_bits = if float_depth { 32 } else { 24 };
        let depth_format = egui_wgpu::depth_format_from_bits(depth_bits, STENCIL_BITS);

        let mut config = WgpuConfiguration::default();
        if let WgpuSetup::CreateNew(setup) = &mut config.wgpu_setup {
            setup.instance_descriptor.backends = wgpu::Backends::GL;
            setup.device_descriptor = Arc::new(|adapter| wgpu::DeviceDescriptor {
                label: Some("solidcraft aros"),
                required_features: adapter.features() & (wgpu::Features::DEPTH32FLOAT_STENCIL8 | wgpu::Features::TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES),
                required_limits: wgpu::Limits::downlevel_defaults().using_resolution(adapter.limits()),
                ..Default::default()
            });
        }
        let options = RendererOptions { msaa_samples: 1, depth_stencil_format: depth_format, ..Default::default() };
        let rs = pollster::block_on(RenderState::create(&config, &instance, Some(&surface), options)).map_err(|e| e.to_string())?;
        let (w, h) = window.inner_size();
        let mut painter = Self { depth_view: None, surface, rs, width: 0, height: 0, depth_bits, depth_format };
        painter.resize(w as u32, h as u32);
        Ok(painter)
    }

    pub fn render_state(&self) -> &RenderState {
        &self.rs
    }

    pub fn depth_bits(&self) -> u8 {
        self.depth_bits
    }

    pub fn msaa_samples(&self) -> u32 {
        self.rs.msaa_samples.max(1)
    }

    /// Configures the surface and the depth buffer for the window's inner size.
    pub fn resize(&mut self, width: u32, height: u32) {
        let (width, height) = (width.max(1), height.max(1));
        if (width, height) == (self.width, self.height) {
            return;
        }
        self.width = width;
        self.height = height;
        let Some(mut cfg) = self.surface.get_default_config(&self.rs.adapter, width, height) else {
            log::error!("the surface is not supported by the adapter");
            return;
        };
        cfg.format = self.rs.target_format;
        cfg.view_formats = vec![self.rs.target_format];
        cfg.present_mode = self.rs.surface_config.present_mode;
        self.surface.configure(&self.rs.device, &cfg);
        self.depth_view = self.depth_format.map(|format| {
            self.rs
                .device
                .create_texture(&wgpu::TextureDescriptor {
                    label: Some("egui depth"),
                    size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format,
                    usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                    view_formats: &[],
                })
                .create_view(&Default::default())
        });
    }

    /// Draws one frame into the window. With `capture`, the frame is drawn into a texture of
    /// its own and its pixels come back (straight RGBA, row by row) instead of being shown.
    pub fn paint(&mut self, clear: [f32; 4], primitives: &[ClippedPrimitive], textures_delta: &mut TexturesDelta, ppp: f32, capture: bool) -> Option<Vec<u8>> {
        let frame = match self.surface.get_current_texture() {
            wgpu::CurrentSurfaceTexture::Success(f) | wgpu::CurrentSurfaceTexture::Suboptimal(f) => f,
            other => {
                log::warn!("no surface texture ({other:?}); configuring the surface again");
                let (w, h) = (self.width, self.height);
                self.width = 0;
                self.resize(w, h);
                return None;
            }
        };
        let device = &self.rs.device;
        let queue = &self.rs.queue;
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("solidcraft frame") });
        let screen = ScreenDescriptor { size_in_pixels: [self.width, self.height], pixels_per_point: ppp };
        let user_cmd_bufs = {
            let mut renderer = self.rs.renderer.write();
            for (id, deltas) in textures_delta.set.drain() {
                for delta in deltas {
                    renderer.update_texture(device, queue, id, &delta);
                }
            }
            renderer.update_buffers(device, queue, &mut encoder, primitives, &screen)
        };
        let capture_texture = capture.then(|| {
            device.create_texture(&wgpu::TextureDescriptor {
                label: Some("solidcraft capture"),
                size: wgpu::Extent3d { width: self.width, height: self.height, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: self.rs.target_format,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            })
        });
        let target_view = match &capture_texture {
            Some(t) => t.create_view(&Default::default()),
            None => frame.texture.create_view(&Default::default()),
        };
        {
            let renderer = self.rs.renderer.read();
            let pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("egui"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &target_view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: clear[0] as f64, g: clear[1] as f64, b: clear[2] as f64, a: clear[3] as f64 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: self.depth_view.as_ref().map(|view| wgpu::RenderPassDepthStencilAttachment {
                    view,
                    depth_ops: self
                        .depth_format
                        .is_some_and(|f| f.has_depth_aspect())
                        .then_some(wgpu::Operations { load: wgpu::LoadOp::Clear(1.0), store: wgpu::StoreOp::Discard }),
                    stencil_ops: self
                        .depth_format
                        .is_some_and(|f| f.has_stencil_aspect())
                        .then_some(wgpu::Operations { load: wgpu::LoadOp::Clear(0), store: wgpu::StoreOp::Discard }),
                }),
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            renderer.render(&mut pass.forget_lifetime(), primitives, &screen);
        }
        // A capture: the pixels, through a buffer with rows padded to wgpu's alignment.
        let bytes_per_row = (self.width * 4).div_ceil(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT) * wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
        let read = capture_texture.as_ref().map(|texture| {
            let buffer = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("solidcraft capture read"),
                size: u64::from(bytes_per_row) * u64::from(self.height),
                usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            encoder.copy_texture_to_buffer(
                texture.as_image_copy(),
                wgpu::TexelCopyBufferInfo {
                    buffer: &buffer,
                    layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(bytes_per_row), rows_per_image: Some(self.height) },
                },
                wgpu::Extent3d { width: self.width, height: self.height, depth_or_array_layers: 1 },
            );
            buffer
        });
        queue.submit(user_cmd_bufs.into_iter().chain([encoder.finish()]));
        {
            let mut renderer = self.rs.renderer.write();
            for id in textures_delta.free.drain() {
                renderer.free_texture(&id);
            }
        }
        queue.present(frame);
        let buffer = read?;
        let slice = buffer.slice(..);
        slice.map_async(wgpu::MapMode::Read, |_| {});
        if let Err(e) = device.poll(wgpu::PollType::wait_indefinitely()) {
            log::error!("capture: {e}");
            return None;
        }
        let data = slice.get_mapped_range().ok()?;
        let bgra = matches!(self.rs.target_format, wgpu::TextureFormat::Bgra8Unorm | wgpu::TextureFormat::Bgra8UnormSrgb);
        let mut rgba = Vec::with_capacity((self.width * self.height * 4) as usize);
        for row in data.chunks(bytes_per_row as usize).take(self.height as usize) {
            for px in row.chunks(4).take(self.width as usize) {
                if bgra {
                    rgba.extend([px[2], px[1], px[0], px[3]]);
                } else {
                    rgba.extend_from_slice(px);
                }
            }
        }
        drop(data);
        buffer.unmap();
        Some(rgba)
    }
}
