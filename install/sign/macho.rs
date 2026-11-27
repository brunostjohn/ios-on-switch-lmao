// SPDX-License-Identifier: GPL-3.0-or-later
use object::LittleEndian as LE;
use object::macho::{self, LinkeditDataCommand, MachHeader64, SegmentCommand64};
use object::pod;
use object::read::macho::{LoadCommandVariant, MachHeader, Section, Segment};

use crate::Error;

#[derive(Clone, Copy)]
pub struct Version {
    pub platform: u32,
    pub min_os: u32,
    pub sdk: u32,
}

#[derive(Clone, Copy)]
pub struct Signature {
    command: usize,
    pub dataoff: u32,
    pub datasize: u32,
}

/// What signing reads from a thin little-endian 64-bit executable.
pub struct Image {
    pub cputype: u32,
    pub filetype: u32,
    pub version: Option<Version>,
    pub text: Option<(u64, u64)>,
    pub info_plist: Option<(usize, usize)>,
    pub signature: Option<Signature>,
    linkedit: (usize, u64, u64),
    first_section: Option<u32>,
    sizeofcmds: u32,
}

impl Image {
    pub fn parse(data: &[u8]) -> Result<Self, Error> {
        let header = MachHeader64::<LE>::parse(data, 0).map_err(|_| Error::Unsupported("not a 64-bit Mach-O"))?;
        if !header.is_little_endian() {
            return Err(Error::Unsupported("a big-endian Mach-O"));
        }
        let mut image = Image {
            cputype: header.cputype.get(LE).0,
            filetype: header.filetype.get(LE).0,
            version: None,
            text: None,
            info_plist: None,
            signature: None,
            linkedit: (0, 0, 0),
            first_section: None,
            sizeofcmds: header.sizeofcmds.get(LE),
        };
        let mut found_linkedit = false;
        let mut min_version = None;
        let mut commands = header
            .load_commands(LE, data, 0)
            .map_err(|_| Error::MachO("load commands"))?;
        while let Some(command) = commands.next().map_err(|_| Error::MachO("load command"))? {
            let at = command.offset() as usize;
            match command.variant().map_err(|_| Error::MachO("load command size"))? {
                LoadCommandVariant::Segment64(segment, sections) => {
                    let sections = segment
                        .sections(LE, sections)
                        .map_err(|_| Error::MachO("segment sections"))?;
                    let (fileoff, filesize) = segment.file_range(LE);
                    if segment.name() == b"__TEXT" && image.text.is_none() {
                        image.text = Some((fileoff, filesize));
                        image.info_plist = sections
                            .iter()
                            .find(|s| s.name() == b"__info_plist")
                            .map(|s| (s.offset.get(LE) as usize, s.size.get(LE) as usize));
                    }
                    if segment.name() == b"__LINKEDIT" {
                        image.linkedit = (at, fileoff, filesize);
                        found_linkedit = true;
                    }
                    if image.first_section.is_none() && fileoff == 0 && filesize != 0 && !sections.is_empty() {
                        image.first_section = Some(sections[0].offset.get(LE));
                    }
                }
                LoadCommandVariant::LinkeditData(data) if command.cmd() == macho::LC_CODE_SIGNATURE => {
                    image.signature.get_or_insert(Signature {
                        command: at,
                        dataoff: data.dataoff.get(LE),
                        datasize: data.datasize.get(LE),
                    });
                }
                LoadCommandVariant::BuildVersion(build, _) if image.version.is_none() => {
                    image.version = Some(Version {
                        platform: build.platform.get(LE).0,
                        min_os: build.minos.get(LE).0,
                        sdk: build.sdk.get(LE).0,
                    });
                }
                LoadCommandVariant::VersionMin(min) if min_version.is_none() => {
                    let platform = match command.cmd() {
                        macho::LC_VERSION_MIN_MACOSX => 1,
                        macho::LC_VERSION_MIN_IPHONEOS => 2,
                        macho::LC_VERSION_MIN_TVOS => 3,
                        macho::LC_VERSION_MIN_WATCHOS => 4,
                        _ => 0,
                    };
                    min_version = Some(Version {
                        platform,
                        min_os: min.version.get(LE).0,
                        sdk: min.sdk.get(LE).0,
                    });
                }
                _ => {}
            }
        }
        if image.version.is_none() {
            image.version = min_version;
        }
        let (_, fileoff, filesize) = image.linkedit;
        if !found_linkedit || fileoff.checked_add(filesize).is_none_or(|end| end > data.len() as u64) {
            return Err(Error::MachO("no __LINKEDIT segment within the file"));
        }
        Ok(image)
    }

    pub fn platform(&self) -> u32 {
        self.version.map_or(0, |v| v.platform)
    }

    /// The file with `space` bytes for a signature at its end, as Security's `codesign_alloc.cpp` makes it: an
    /// existing signature keeps its offset, a new one goes at the file's end rounded up to 16, and `__LINKEDIT`
    /// covers it with its VM size rounded up to 16 KiB. Bytes past the input stay zero; old signature bytes past
    /// the new blob are kept.
    pub fn allocate(&self, input: &[u8], space: u32) -> Result<(Vec<u8>, u32), Error> {
        let mut out = input.to_vec();
        let dataoff = match self.signature {
            Some(signature) => {
                let end = u64::from(signature.dataoff) + u64::from(signature.datasize);
                if end + 7 < input.len() as u64 {
                    return Err(Error::MachO("the signature is not at the end of the file"));
                }
                let (command, _) = pod::from_bytes_mut::<LinkeditDataCommand<LE>>(&mut out[signature.command..])
                    .map_err(|_| Error::MachO("LC_CODE_SIGNATURE"))?;
                command.datasize.set(LE, space);
                signature.dataoff
            }
            None => self.append_signature_command(&mut out, input.len(), space)?,
        };
        let new_len = u64::from(dataoff) + u64::from(space);
        let new_len = u32::try_from(new_len).map_err(|_| Error::Unsupported("a signed file over 4 GiB"))?;
        out.resize(new_len as usize, 0);
        let (at, fileoff, _) = self.linkedit;
        let (segment, _) =
            pod::from_bytes_mut::<SegmentCommand64<LE>>(&mut out[at..]).map_err(|_| Error::MachO("__LINKEDIT"))?;
        let filesize = u64::from(new_len) - fileoff;
        segment.filesize.set(LE, filesize);
        segment.vmsize.set(LE, filesize.next_multiple_of(0x4000));
        Ok((out, dataoff))
    }

    // Security checks the room against the load commands' size alone; this counts the header as well, so it never
    // writes over a section that Security would.
    fn append_signature_command(&self, out: &mut [u8], input_len: usize, space: u32) -> Result<u32, Error> {
        let end = 32 + self.sizeofcmds as usize;
        let room = self.first_section.map_or(out.len(), |offset| offset as usize);
        if end + 16 > room || end + 16 > out.len() {
            return Err(Error::Unsupported("no room in the header for LC_CODE_SIGNATURE"));
        }
        let dataoff = u32::try_from(input_len.next_multiple_of(16)).map_err(|_| Error::Unsupported("over 4 GiB"))?;
        for (index, word) in [macho::LC_CODE_SIGNATURE.0, 16, dataoff, space].into_iter().enumerate() {
            out[end + 4 * index..end + 4 * index + 4].copy_from_slice(&word.to_le_bytes());
        }
        let (header, _) = pod::from_bytes_mut::<MachHeader64<LE>>(out).map_err(|_| Error::MachO("header"))?;
        header.ncmds.set(LE, header.ncmds.get(LE) + 1);
        header.sizeofcmds.set(LE, self.sizeofcmds + 16);
        Ok(dataoff)
    }
}
