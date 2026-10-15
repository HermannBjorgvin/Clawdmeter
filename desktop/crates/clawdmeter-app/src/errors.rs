//! Short, human error text for the windows. Technical detail goes to the
//! log, not the UI.

use std::io;
use std::path::Path;

/// One line for an I/O failure, without the OS error code.
pub fn io_message(e: &io::Error) -> String {
    match e.kind() {
        io::ErrorKind::PermissionDenied => "Write permission denied.".into(),
        io::ErrorKind::NotFound => "The file or folder was not found.".into(),
        io::ErrorKind::ReadOnlyFilesystem => "The disk is read-only.".into(),
        io::ErrorKind::StorageFull => "The disk is full.".into(),
        _ => {
            // Drop the " (os error 13)" suffix std adds.
            let s = e.to_string();
            match s.rfind(" (os error") {
                Some(i) => s[..i].to_owned(),
                None => s,
            }
        }
    }
}

/// "Couldn't save ~/x/config: Write permission denied."
pub fn save_failed(what: &str, path: &Path, home: &Path, e: &io::Error) -> String {
    format!("Couldn't save {what} ({}). {}", crate::tilde(&path.display().to_string(), home), io_message(e))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn messages() {
        assert_eq!(io_message(&io::Error::from(io::ErrorKind::PermissionDenied)), "Write permission denied.");
        assert_eq!(io_message(&io::Error::from_raw_os_error(28)), "The disk is full.");
        let s = save_failed(
            "settings",
            Path::new("/home/u/.config/x"),
            Path::new("/home/u"),
            &io::Error::from(io::ErrorKind::PermissionDenied),
        );
        assert_eq!(s, "Couldn't save settings (~/.config/x). Write permission denied.");
    }
}
