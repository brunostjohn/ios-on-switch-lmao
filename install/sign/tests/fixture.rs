// SPDX-License-Identifier: GPL-3.0-or-later
//! Small unsigned executables for codesign to sign.

pub const ARM64: (u32, u32) = (0x0100_000c, 0);
pub const X86_64: (u32, u32) = (0x0100_0007, 3);

#[derive(Clone, Copy)]
pub enum Version {
    None,
    Build { platform: u32, min_os: u32, sdk: u32 },
    Min { command: u32, min_os: u32, sdk: u32 },
}

#[derive(Clone)]
pub struct Fixture {
    pub cpu: (u32, u32),
    pub version: Version,
    pub text_size: u64,
    pub linkedit_size: u64,
    pub linkedit_vmsize: u64,
    pub info_plist: Option<Vec<u8>>,
}

impl Fixture {
    pub fn ios(text_size: u64, linkedit_size: u64) -> Self {
        Fixture {
            cpu: ARM64,
            version: Version::Build {
                platform: 2,
                min_os: 0x11_0000,
                sdk: 0x11_0000,
            },
            text_size,
            linkedit_size,
            linkedit_vmsize: linkedit_size.next_multiple_of(0x4000),
            info_plist: None,
        }
    }

    pub fn build(&self) -> Vec<u8> {
        const BASE: u64 = 0x1_0000_0000;
        const TEXT_START: u64 = 0x1000;
        let mut sections = vec![section(
            b"__text",
            BASE + TEXT_START,
            self.text_size - TEXT_START,
            TEXT_START,
        )];
        if let Some(info) = &self.info_plist {
            sections.push(section(b"__info_plist", BASE + 0x800, info.len() as u64, 0x800));
        }
        let mut commands = vec![
            segment(b"__PAGEZERO", (0, BASE), (0, 0), 0, &[]),
            segment(b"__TEXT", (BASE, self.text_size), (0, self.text_size), 5, &sections),
            segment(
                b"__LINKEDIT",
                (BASE + self.text_size, self.linkedit_vmsize),
                (self.text_size, self.linkedit_size),
                1,
                &[],
            ),
            words(&[
                0x2,
                24,
                self.text_size as u32,
                0,
                self.text_size as u32,
                self.linkedit_size as u32,
            ]),
        ];
        match self.version {
            Version::None => {}
            Version::Build { platform, min_os, sdk } => commands.push(words(&[0x32, 24, platform, min_os, sdk, 0])),
            Version::Min { command, min_os, sdk } => commands.push(words(&[command, 16, min_os, sdk])),
        }
        commands.push(words(&[0x8000_0028, 24, TEXT_START as u32, 0, 0, 0]));
        let mut dylinker = words(&[0xe, 32, 12]);
        dylinker.extend_from_slice(b"/usr/lib/dyld\0");
        dylinker.resize(32, 0);
        commands.push(dylinker);
        let commands = commands.concat();

        let mut out = vec![0u8; (self.text_size + self.linkedit_size) as usize];
        let header = words(&[
            0xfeed_facf,
            self.cpu.0,
            self.cpu.1,
            2,
            0,
            commands.len() as u32,
            0x0020_0085,
            0,
        ]);
        out[..32].copy_from_slice(&header);
        out[16..20].copy_from_slice(&(command_count(&commands) as u32).to_le_bytes());
        out[32..32 + commands.len()].copy_from_slice(&commands);
        if let Some(info) = &self.info_plist {
            out[0x800..0x800 + info.len()].copy_from_slice(info);
        }
        for (index, byte) in out[TEXT_START as usize..].iter_mut().enumerate() {
            *byte = (index.wrapping_mul(2_654_435_761) >> 13) as u8 | 1;
        }
        out
    }
}

fn command_count(commands: &[u8]) -> usize {
    let mut at = 0;
    let mut count = 0;
    while at < commands.len() {
        at += u32::from_le_bytes(commands[at + 4..at + 8].try_into().unwrap()) as usize;
        count += 1;
    }
    count
}

fn words(values: &[u32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

fn name(text: &[u8]) -> [u8; 16] {
    let mut out = [0; 16];
    out[..text.len()].copy_from_slice(text);
    out
}

fn segment(segname: &[u8], vm: (u64, u64), file: (u64, u64), prot: u32, sections: &[Vec<u8>]) -> Vec<u8> {
    let mut out = words(&[0x19, 72 + 80 * sections.len() as u32]);
    out.extend_from_slice(&name(segname));
    for value in [vm.0, vm.1, file.0, file.1] {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out.extend(words(&[prot, prot, sections.len() as u32, 0]));
    for section in sections {
        let mut section = section.clone();
        section[16..32].copy_from_slice(&name(segname));
        out.extend(section);
    }
    out
}

fn section(sectname: &[u8], addr: u64, size: u64, offset: u64) -> Vec<u8> {
    let mut out = name(sectname).to_vec();
    out.extend_from_slice(&[0; 16]);
    out.extend_from_slice(&addr.to_le_bytes());
    out.extend_from_slice(&size.to_le_bytes());
    out.extend(words(&[offset as u32, 2, 0, 0, 0x8000_0400, 0, 0, 0]));
    out
}
