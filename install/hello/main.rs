// SPDX-License-Identifier: GPL-3.0-or-later
use std::error::Error;
use std::io::stdout;

use crossterm::tty::IsTty;
use ratatui::text::Line;
use ratatui::{TerminalOptions, Viewport};

const RELEASES: &str = "https://api.github.com/repos/brunostjohn/ios-on-switch-lmao/releases";

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), Box<dyn Error>> {
    let url = std::env::args().nth(1).unwrap_or_else(|| RELEASES.to_owned());
    let releases: Vec<serde_json::Value> = reqwest::Client::builder()
        .user_agent("switch-installer-hello")
        .build()?
        .get(url)
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?;
    let count = format!("{} releases", releases.len());
    if stdout().is_tty() {
        let mut terminal = ratatui::init_with_options(TerminalOptions {
            viewport: Viewport::Inline(1),
        });
        let drawn = terminal
            .draw(|frame| frame.render_widget(Line::raw(count), frame.area()))
            .map(drop);
        ratatui::restore();
        drawn?;
        println!();
    } else {
        println!("{count}");
    }
    Ok(())
}
