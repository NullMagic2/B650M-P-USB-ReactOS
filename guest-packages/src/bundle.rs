//! Module: lightning.guest_packages.bundle
//! Purpose: Produce a standalone executable from the same verified package snapshot.
//! Created: 2026-10-25
//! Architecture: The unchanged native backend reads a bounded, hashed executable overlay.
use crate::{relative, snapshot, MAX_TOTAL_PAYLOAD_BYTES, SETUP_NAME};
use anyhow::{ensure, Context, Result};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeSet,
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};

const MAGIC: &[u8; 8] = b"QLPKG001";
const MAX_ENTRIES: usize = 1024;
const MAX_BUNDLE_BYTES: u64 = MAX_TOTAL_PAYLOAD_BYTES + 1024 * 1024;

fn entries(root: &Path, current: &Path, paths: &mut Vec<PathBuf>) -> Result<()> {
    for entry in fs::read_dir(current)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        ensure!(!kind.is_symlink(), "Bundle snapshot contains a symlink");
        if kind.is_dir() {
            entries(root, &entry.path(), paths)?;
        } else {
            ensure!(kind.is_file(), "Invalid bundle snapshot entry");
            let name = entry.path().strip_prefix(root)?.to_path_buf();
            if name != Path::new(SETUP_NAME) {
                paths.push(name);
                ensure!(paths.len() <= MAX_ENTRIES, "Too many bundle entries");
            }
        }
    }
    Ok(())
}

fn file_hash(path: &Path) -> Result<[u8; 32]> {
    let mut file = fs::File::open(path)?;
    let mut sha = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let n = file.read(&mut buffer)?;
        if n == 0 {
            break;
        }
        sha.update(&buffer[..n]);
    }
    Ok(sha.finalize().into())
}

/// Build one offline installer, without a manager, CD or companion payload files.
/// The embedded plan uses exactly the dependency and baseline rules of CD setup.
pub fn prepare_executable(
    root: &Path,
    selected: &[String],
    setup: &Path,
    output: &Path,
) -> Result<()> {
    let staging = snapshot(root, selected, setup, output)?;
    let mut paths = Vec::new();
    entries(staging.path(), staging.path(), &mut paths)?;
    paths.sort();
    let mut seen = BTreeSet::new();
    let mut body = tempfile::NamedTempFile::new_in(output.parent().context("No output parent")?)?;
    body.write_all(&(paths.len() as u32).to_le_bytes())?;
    for path in paths {
        let name = path.to_str().context("Bundle path is not UTF-8")?;
        ensure!(
            relative(name) && seen.insert(name.to_ascii_lowercase()),
            "Unsafe or duplicate bundle path"
        );
        let source = staging.path().join(&path);
        let size = fs::metadata(&source)?.len();
        body.write_all(&(name.len() as u32).to_le_bytes())?;
        body.write_all(&size.to_le_bytes())?;
        body.write_all(&file_hash(&source)?)?;
        body.write_all(name.as_bytes())?;
        std::io::copy(&mut fs::File::open(source)?, &mut body)?;
        ensure!(
            body.as_file().metadata()?.len() <= MAX_BUNDLE_BYTES,
            "Bundle exceeds its size limit"
        );
    }
    body.flush()?;
    let mut result = tempfile::NamedTempFile::new_in(output.parent().unwrap())?;
    let stub = staging.path().join(SETUP_NAME);
    let mut input = fs::File::open(&stub)?;
    let mut signature = [0u8; 2];
    input.read_exact(&mut signature)?;
    ensure!(
        &signature == b"MZ",
        "Native setup backend is not an executable"
    );
    let offset = std::io::copy(&mut fs::File::open(stub)?, &mut result)?;
    std::io::copy(&mut fs::File::open(body.path())?, &mut result)?;
    result.write_all(MAGIC)?;
    result.write_all(&offset.to_le_bytes())?;
    result.write_all(&body.as_file().metadata()?.len().to_le_bytes())?;
    result.write_all(&file_hash(body.path())?)?;
    result.flush()?;
    result.as_file().sync_all()?;
    result.persist(output).map_err(|error| error.error)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Catalog, Package, Payload};
    #[test]
    fn standalone_bundle_retains_verified_plan_and_offline_payloads() {
        let root = tempfile::tempdir().unwrap();
        let bytes = b"generic component";
        fs::write(root.path().join("component.bin"), bytes).unwrap();
        let payload = Payload {
            source: "component.bin".into(),
            sha256: format!("{:x}", Sha256::digest(bytes)),
            destination: String::new(),
            previous_sha256: vec![],
            reboot: false,
            boot_service: String::new(),
        };
        let catalog = Catalog {
            schema: 1,
            packages: vec![Package {
                id: "generic-component".into(),
                label: "Generic component".into(),
                version: "1".into(),
                os: "reactos".into(),
                arch: "i386".into(),
                requires: vec![],
                required_versions: Default::default(),
                license: payload.clone(),
                files: vec![payload],
                drivers: vec![],
            }],
        };
        fs::write(
            root.path().join("catalog.json"),
            serde_json::to_vec(&catalog).unwrap(),
        )
        .unwrap();
        let stub = root.path().join("backend.exe");
        fs::write(&stub, b"MZgeneric-backend").unwrap();
        let output = root.path().join("standalone.exe");
        prepare_executable(root.path(), &["generic-component".into()], &stub, &output).unwrap();
        let data = fs::read(output).unwrap();
        let trailer = &data[data.len() - 56..];
        assert_eq!(&trailer[..8], MAGIC);
        let offset = u64::from_le_bytes(trailer[8..16].try_into().unwrap()) as usize;
        let size = u64::from_le_bytes(trailer[16..24].try_into().unwrap()) as usize;
        assert_eq!(offset, b"MZgeneric-backend".len());
        assert_eq!(offset + size, data.len() - 56);
        assert_eq!(
            &Sha256::digest(&data[offset..offset + size])[..],
            &trailer[24..]
        );
        let mut cursor = offset;
        let count = u32::from_le_bytes(data[cursor..cursor + 4].try_into().unwrap());
        cursor += 4;
        let mut names = Vec::new();
        for _ in 0..count {
            let n = u32::from_le_bytes(data[cursor..cursor + 4].try_into().unwrap()) as usize;
            let length =
                u64::from_le_bytes(data[cursor + 4..cursor + 12].try_into().unwrap()) as usize;
            let hash = &data[cursor + 12..cursor + 44];
            cursor += 44;
            names.push(
                std::str::from_utf8(&data[cursor..cursor + n])
                    .unwrap()
                    .to_owned(),
            );
            cursor += n;
            assert_eq!(&Sha256::digest(&data[cursor..cursor + length])[..], hash);
            cursor += length;
        }
        assert_eq!(cursor, offset + size);
        assert!(names.contains(&"setup.ini".into()));
        assert!(names.contains(&"generic-component/component.bin".into()));
        assert!(names.contains(&"generic-component/LICENSE.txt".into()));
        assert!(!names.contains(&SETUP_NAME.into()));
        fs::write(root.path().join("component.bin"), b"unexpected").unwrap();
        assert!(prepare_executable(
            root.path(),
            &["generic-component".into()],
            &stub,
            &root.path().join("bad.exe")
        )
        .is_err());
    }
}
