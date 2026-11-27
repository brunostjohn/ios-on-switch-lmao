// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use switch_fetch::Url;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Normal,
    /// 200 with the whole file, whatever the Range.
    IgnoreRange,
    /// A 206 whose Content-Range is one byte off.
    ShiftRange,
    /// Declares the full length, sends this many bytes, then closes.
    CutAfter(usize),
    /// No Content-Length: sends this many bytes and closes, which reads as a clean end of body.
    CloseAfter(usize),
    /// Declares the full length, sends this many bytes, then hangs.
    StallAfter(usize),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    pub path: String,
    pub range: Option<(u64, Option<u64>)>,
}

#[derive(Default)]
struct Shared {
    files: HashMap<String, Vec<u8>>,
    mode: Option<Mode>,
    log: Vec<Request>,
}

pub struct Server {
    port: u16,
    shared: Arc<Mutex<Shared>>,
}

impl Server {
    pub fn start(files: &[(&str, &[u8])]) -> Server {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let shared = Arc::new(Mutex::new(Shared {
            files: files
                .iter()
                .map(|(name, data)| (format!("/{name}"), data.to_vec()))
                .collect(),
            ..Shared::default()
        }));
        let state = shared.clone();
        thread::spawn(move || {
            for stream in listener.incoming().flatten() {
                let state = state.clone();
                thread::spawn(move || serve(stream, &state));
            }
        });
        Server { port, shared }
    }

    pub fn url(&self, name: &str) -> Url {
        Url::parse(&format!("http://127.0.0.1:{}/{name}", self.port)).unwrap()
    }

    pub fn set_mode(&self, mode: Mode) {
        self.shared.lock().unwrap().mode = Some(mode);
    }

    pub fn requests(&self) -> Vec<Request> {
        self.shared.lock().unwrap().log.clone()
    }

    pub fn range_starts(&self) -> Vec<u64> {
        self.requests()
            .iter()
            .filter_map(|r| r.range.map(|(first, _)| first))
            .collect()
    }
}

fn parse_range(value: &str) -> Option<(u64, Option<u64>)> {
    let (first, last) = value.trim().strip_prefix("bytes=")?.split_once('-')?;
    Some((
        first.parse().ok()?,
        if last.is_empty() {
            None
        } else {
            Some(last.parse().ok()?)
        },
    ))
}

fn serve(stream: TcpStream, shared: &Mutex<Shared>) {
    let mut reader = BufReader::new(stream.try_clone().unwrap());
    let mut line = String::new();
    reader.read_line(&mut line).unwrap();
    let path = line.split(' ').nth(1).unwrap_or("/").to_owned();
    let mut range = None;
    loop {
        let mut header = String::new();
        if reader.read_line(&mut header).unwrap() == 0 || header.trim().is_empty() {
            break;
        }
        if let Some((name, value)) = header.split_once(':')
            && name.eq_ignore_ascii_case("range")
        {
            range = parse_range(value);
        }
    }
    let (body, mode) = {
        let mut shared = shared.lock().unwrap();
        shared.log.push(Request {
            path: path.clone(),
            range,
        });
        (shared.files.get(&path).cloned(), shared.mode.unwrap_or(Mode::Normal))
    };
    respond(stream, body, range, mode);
}

fn respond(mut stream: TcpStream, body: Option<Vec<u8>>, range: Option<(u64, Option<u64>)>, mode: Mode) {
    let Some(body) = body else {
        let _ = stream.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    };
    let total = body.len() as u64;
    let mut head = String::new();
    let (first, last) = match range {
        Some((first, last)) if mode != Mode::IgnoreRange => {
            let last = last.unwrap_or(total - 1).min(total - 1);
            let shift = u64::from(mode == Mode::ShiftRange);
            head.push_str("HTTP/1.1 206 Partial Content\r\n");
            head.push_str(&format!(
                "Content-Range: bytes {}-{}/{total}\r\n",
                first + shift,
                last + shift
            ));
            (first, last)
        }
        _ => {
            head.push_str("HTTP/1.1 200 OK\r\n");
            (0, total - 1)
        }
    };
    let slice = &body[first as usize..=last as usize];
    if !matches!(mode, Mode::CloseAfter(_)) {
        head.push_str(&format!("Content-Length: {}\r\n", slice.len()));
    }
    head.push_str("Connection: close\r\n\r\n");
    let sent = match mode {
        Mode::CutAfter(n) | Mode::CloseAfter(n) | Mode::StallAfter(n) => &slice[..n.min(slice.len())],
        _ => slice,
    };
    let _ = stream
        .write_all(head.as_bytes())
        .and_then(|()| stream.write_all(sent))
        .and_then(|()| stream.flush());
    if let Mode::StallAfter(_) = mode {
        thread::sleep(Duration::from_secs(60));
    }
}

/// Deterministic bytes that neither repeat nor compress.
pub fn noise(len: usize, seed: u64) -> Vec<u8> {
    let mut x = seed | 1;
    (0..len)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x >> 32) as u8
        })
        .collect()
}

pub fn sha256(data: &[u8]) -> [u8; 32] {
    use sha2::Digest;
    sha2::Sha256::digest(data).into()
}
