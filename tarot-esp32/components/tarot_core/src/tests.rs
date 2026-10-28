#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn audio_header_is_stable() {
        assert_eq!(std::mem::size_of::<AudioHeader>(), 20);
        assert_eq!(std::mem::align_of::<AudioHeader>(), 1);
    }

    #[test]
    fn audio_constants_are_safe_defaults() {
        assert_eq!(AUDIO_MAGIC, b"AUD0");
        assert_eq!(AUDIO_VERSION, 1);
    }
}
