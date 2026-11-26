// SPDX-License-Identifier: GPL-3.0-or-later
//! The geometry sweep's helper. `rule` prints the geometry `switch_apfs_alloc` gives a block count and `read` the
//! geometry a container image has, in the same words, so the sweep compares lines. `gpt` partitions an image for
//! `diskutil apfs resizeContainer`, and `bump-limit` makes the negative controls.

mod gpt;

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand, ValueEnum};
use switch_apfs_alloc::Geometry;
use switch_apfs_format::io::{BlockTarget, FileSource, FileTarget};
use switch_apfs_format::spaceman::{SD_MAIN, SFQ_IP, SFQ_MAIN, SpacemanPhys};
use switch_apfs_format::{get, put, seal};
use switch_apfs_read::Container;
use zerocopy::little_endian::U16;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Rule {
        blocks: u64,
    },
    Read {
        image: PathBuf,
        #[arg(long, default_value_t = 0)]
        offset: u64,
    },
    /// A GPT with one Apple_APFS partition of `sectors` 512-byte sectors from `first`.
    Gpt {
        image: PathBuf,
        #[arg(long)]
        first: u64,
        #[arg(long)]
        sectors: u64,
    },
    /// One free queue's node limit one past the size rule, the space manager resealed in place.
    BumpLimit {
        image: PathBuf,
        #[arg(value_enum)]
        queue: Queue,
    },
}

#[derive(Clone, Copy, ValueEnum)]
enum Queue {
    Pool,
    Main,
}

struct Line {
    blocks: u64,
    desc: u64,
    data: u64,
    max_fs: u32,
    chunks: u64,
    cibs: u64,
    cabs: u32,
    pool: u64,
    pool_limit: u16,
    main_limit: u16,
}

impl std::fmt::Display for Line {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(
            f,
            "blocks={} desc={} data={} max_fs={} chunks={} cibs={} cabs={} pool={} pool_limit={} main_limit={}",
            self.blocks,
            self.desc,
            self.data,
            self.max_fs,
            self.chunks,
            self.cibs,
            self.cabs,
            self.pool,
            self.pool_limit,
            self.main_limit
        )
    }
}

fn main() -> ExitCode {
    match run(Cli::parse().command) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_apfs_alloc_sweep: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(command: Command) -> Result<()> {
    match command {
        Command::Rule { blocks } => println!("{}", rule(blocks)?),
        Command::Read { image, offset } => println!("{}", read(&image, offset)?),
        Command::Gpt { image, first, sectors } => gpt::write(&image, first, sectors)?,
        Command::BumpLimit { image, queue } => bump_limit(&image, queue)?,
    }
    Ok(())
}

fn rule(blocks: u64) -> Result<Line> {
    let g = Geometry::for_blocks(blocks)?;
    Ok(Line {
        blocks,
        desc: g.checkpoint.desc_blocks.into(),
        data: g.checkpoint.data_blocks.into(),
        max_fs: g.max_file_systems,
        chunks: g.chunks,
        cibs: g.cibs,
        cabs: 0,
        pool: g.pool_blocks,
        pool_limit: g.pool_queue_limit,
        main_limit: g.main_queue_limit,
    })
}

fn read(image: &PathBuf, offset: u64) -> Result<Line> {
    let c = Container::open(&FileSource::open(image, offset)?)?;
    let cp = &c.checkpoint;
    let sm = &c.space.spaceman;
    let dev = &sm.dev[SD_MAIN];
    Ok(Line {
        blocks: c.blocks(),
        desc: cp.desc_area.end - cp.desc_area.start,
        data: cp.data_area.end - cp.data_area.start,
        max_fs: cp.nx.max_file_systems.get(),
        chunks: dev.chunk_count.get(),
        cibs: dev.cib_count.get().into(),
        cabs: dev.cab_count.get(),
        pool: sm.ip_block_count.get(),
        pool_limit: sm.fq[SFQ_IP].tree_node_limit.get(),
        main_limit: sm.fq[SFQ_MAIN].tree_node_limit.get(),
    })
}

fn bump_limit(image: &PathBuf, queue: Queue) -> Result<()> {
    let mut target = FileTarget::open(image)?;
    let c = Container::open(&target)?;
    let (paddr, block) = c.checkpoint.ephemeral(c.checkpoint.nx.spaceman_oid.get())?;
    let mut block = block.clone();
    let mut sm: SpacemanPhys = get(&block[..], 0).ok_or("short space manager")?;
    let q = match queue {
        Queue::Pool => SFQ_IP,
        Queue::Main => SFQ_MAIN,
    };
    let limit = sm.fq[q].tree_node_limit.get();
    sm.fq[q].tree_node_limit = U16::new(limit + 1);
    put(&mut block[..], 0, &sm);
    seal(&mut block[..]);
    target.write_blocks(*paddr, &block[..])?;
    target.flush()?;
    println!("queue limit {limit} -> {}", limit + 1);
    Ok(())
}
