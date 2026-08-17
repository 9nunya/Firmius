use std::borrow::Cow;
use std::io::Cursor;

use arboard::Clipboard;
use base64::{Engine as _, engine::general_purpose::STANDARD};
use image::ColorType;
use image::ImageEncoder;
use image::codecs::png::PngEncoder;

use super::composer::{PastedImage, StoredPaste};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClipboardPaste {
    Text(String),
    Image(PastedImage),
}

pub fn read_clipboard_paste() -> Result<ClipboardPaste, String> {
    let mut clipboard = Clipboard::new().map_err(|error| error.to_string())?;
    if let Ok(image) = clipboard.get_image() {
        return rgba_image_to_paste(image.bytes, image.width, image.height);
    }
    let text = clipboard.get_text().map_err(|error| error.to_string())?;
    Ok(ClipboardPaste::Text(text))
}

fn rgba_image_to_paste(
    bytes: Cow<'_, [u8]>,
    width: usize,
    height: usize,
) -> Result<ClipboardPaste, String> {
    let mut png = Vec::new();
    PngEncoder::new(Cursor::new(&mut png))
        .write_image(
            bytes.as_ref(),
            width as u32,
            height as u32,
            ColorType::Rgba8.into(),
        )
        .map_err(|error| error.to_string())?;
    Ok(ClipboardPaste::Image(PastedImage {
        media_type: "image/png".to_string(),
        data_base64: STANDARD.encode(png),
        width,
        height,
        bytes: bytes.len(),
    }))
}

pub fn into_stored_paste(paste: ClipboardPaste) -> StoredPaste {
    match paste {
        ClipboardPaste::Text(text) => StoredPaste::Text(text),
        ClipboardPaste::Image(image) => StoredPaste::Image(image),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rgba_clipboard_image_converts_to_png_base64() {
        let bytes = Cow::Owned(vec![255, 0, 0, 255]);
        let ClipboardPaste::Image(image) = rgba_image_to_paste(bytes, 1, 1).unwrap() else {
            panic!("expected image paste")
        };
        assert_eq!(image.media_type, "image/png");
        assert_eq!(image.width, 1);
        assert_eq!(image.height, 1);
        assert_eq!(image.bytes, 4);
        assert!(!image.data_base64.is_empty());
    }
}
