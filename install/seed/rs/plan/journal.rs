// SPDX-License-Identifier: GPL-3.0-or-later

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Stage {
    pub name: String,
    pub lib: Option<String>,
    pub spec: bool,
    pub constants: Vec<String>,
    pub specialized_name: Option<String>,
}

impl Stage {
    pub fn new(name: impl Into<String>, lib: Option<String>) -> Self {
        Self {
            name: name.into(),
            lib,
            spec: false,
            constants: Vec::new(),
            specialized_name: None,
        }
    }

    pub fn text(&self) -> String {
        let mut parts = vec![self.name.clone()];
        if let Some(lib) = &self.lib {
            parts.push(format!("lib={lib}"));
        }
        if let Some(spec_name) = &self.specialized_name {
            parts.push(format!("as={spec_name}"));
        }
        if self.spec && self.constants.is_empty() {
            parts.push("spec".to_owned());
        }
        parts.extend(self.constants.iter().cloned());
        parts.join(" ")
    }

    pub fn specialized(&self, constants: &[String]) -> Self {
        Self {
            name: self.name.clone(),
            lib: self.lib.clone(),
            spec: true,
            constants: ordered(constants),
            specialized_name: self.specialized_name.clone(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct RenderKey {
    pub target: Vec<String>,
    pub layout: Vec<String>,
    pub vertex: Stage,
    pub fragment: Option<Stage>,
    pub linked: Vec<(String, Stage)>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Render {
    pub target: Vec<String>,
    pub layout: Vec<String>,
    pub vertex: Stage,
    pub fragment: Option<Stage>,
    pub label: String,
    pub linked: Vec<(String, Stage)>,
}

impl Render {
    pub fn text(&self) -> String {
        let head = self
            .target
            .iter()
            .chain(self.layout.iter())
            .cloned()
            .collect::<Vec<_>>()
            .join(" ");
        let fragment = self.fragment.as_ref().map_or_else(|| "-".to_owned(), Stage::text);
        let mut linked = String::new();
        for (tag, stage) in &self.linked {
            linked.push_str(tag);
            linked.push(' ');
            linked.push_str(&stage.text());
            linked.push_str(" | ");
        }
        format!(
            "R ok=1 label={} {head} | v {} | f {fragment} | {linked}",
            self.label,
            self.vertex.text()
        )
    }

    pub fn key(&self) -> RenderKey {
        RenderKey {
            target: self.target.clone(),
            layout: self.layout.clone(),
            vertex: self.vertex.clone(),
            fragment: self.fragment.clone(),
            linked: self.linked.clone(),
        }
    }

    pub fn colors(&self) -> usize {
        self.target.iter().filter(|t| t.starts_with('c')).count()
    }
}

pub fn constant_key(token: &str) -> &str {
    let mut colons = token.rmatch_indices(':');
    let _last = colons.next();
    match colons.next() {
        Some((idx, _)) => &token[..idx],
        None => token,
    }
}

pub fn constant_type_str(token: &str) -> Option<&str> {
    let mut colons = token.rmatch_indices(':');
    let (last_idx, _) = colons.next()?;
    let (prev_idx, _) = colons.next()?;
    Some(&token[prev_idx + 1..last_idx])
}

pub fn ordered(constants: &[String]) -> Vec<String> {
    let mut out = constants.to_vec();
    out.sort_by(|a, b| order_key(a).cmp(&order_key(b)));
    out
}

fn order_key(token: &str) -> (u8, u64, &str) {
    let key = constant_key(token);
    if let Some(rest) = key.strip_prefix('#')
        && !rest.is_empty()
        && rest.bytes().all(|b| b.is_ascii_digit())
        && let Ok(idx) = rest.parse::<u64>()
    {
        return (0, idx, "");
    }
    (1, 0, key)
}

fn is_target_field(token: &str) -> bool {
    if token.starts_with("samples=")
        || token.starts_with("topo=")
        || token.starts_with("ds=")
        || token.starts_with("opt=")
    {
        return true;
    }
    if let Some(rest) = token.strip_prefix('c')
        && let Some(eq) = rest.find('=')
    {
        return eq > 0 && rest[..eq].bytes().all(|b| b.is_ascii_digit());
    }
    false
}

fn is_layout_field(token: &str) -> bool {
    if let Some(rest) = token.strip_prefix('a')
        && let Some(eq) = rest.find('=')
    {
        return eq > 0 && rest[..eq].bytes().all(|b| b.is_ascii_digit());
    }
    false
}

pub fn parse_stage(text: &str) -> Option<Stage> {
    let mut tokens = text.split_ascii_whitespace();
    let first = tokens.next()?;
    if first == "-" {
        return None;
    }
    let mut lib = None;
    let mut spec = false;
    let mut constants = Vec::new();
    let mut specialized_name = None;
    for token in tokens {
        if let Some(v) = token.strip_prefix("lib=") {
            lib = Some(v.to_owned());
        } else if let Some(v) = token.strip_prefix("as=") {
            specialized_name = Some(v.to_owned());
        } else if token == "spec" {
            spec = true;
        } else if token.bytes().filter(|&b| b == b':').count() >= 2 {
            constants.push(token.to_owned());
        }
    }
    Some(Stage {
        name: first.to_owned(),
        lib,
        spec: spec || !constants.is_empty(),
        constants,
        specialized_name,
    })
}

pub fn parse_render(line: &str) -> Option<Render> {
    if !line.starts_with("R ") {
        return None;
    }
    let parts: Vec<&str> = line.split(" | ").collect();
    if parts.len() < 3 || !parts[1].starts_with("v ") || !parts[2].starts_with("f ") {
        return None;
    }
    let head: Vec<&str> = parts[0].split_ascii_whitespace().collect();
    let vertex = parse_stage(&parts[1][2..])?;
    let label = head
        .iter()
        .find_map(|t| t.strip_prefix("label="))
        .unwrap_or("-")
        .to_owned();
    let mut linked = Vec::new();
    for &part in &parts[3..] {
        if matches!(part.get(..3), Some("vl " | "vp " | "fl " | "fp "))
            && let Some(stage) = parse_stage(&part[3..])
        {
            linked.push((part[..2].to_owned(), stage));
        }
    }
    Some(Render {
        target: head
            .iter()
            .copied()
            .filter(|t| is_target_field(t))
            .map(str::to_owned)
            .collect(),
        layout: head
            .iter()
            .copied()
            .filter(|t| is_layout_field(t))
            .map(str::to_owned)
            .collect(),
        vertex,
        fragment: parse_stage(&parts[2][2..]),
        label,
        linked,
    })
}

pub fn parse_kernel(line: &str) -> Option<(String, Stage)> {
    let owned;
    let effective = if let Some(rest) = line.strip_prefix("D ") {
        if let Some((_, after)) = rest.split_once(" | ") {
            owned = format!("C ok=1 {after}");
            owned.as_str()
        } else {
            line
        }
    } else {
        line
    };
    if !effective.starts_with("C ") {
        return None;
    }
    let parts: Vec<&str> = effective.split(" | ").collect();
    if parts.len() < 2 || !parts[1].starts_with("k ") {
        return None;
    }
    let kernel = parse_stage(&parts[1][2..])?;
    let fields = parts[0]
        .split_ascii_whitespace()
        .skip(2)
        .filter(|t| !t.starts_with("label="))
        .collect::<Vec<_>>()
        .join(" ");
    Some((fields, kernel))
}

pub fn kernel_links(line: &str) -> Vec<(String, Stage)> {
    line.split(" | ")
        .skip(1)
        .filter_map(|part| {
            if matches!(part.get(..2), Some("l " | "p ")) {
                parse_stage(&part[2..]).map(|stage| (part[..1].to_owned(), stage))
            } else {
                None
            }
        })
        .collect()
}

pub fn compute_line(fields: &str, kernel: &Stage) -> String {
    format!("C ok=1 {fields} | k {} | ", kernel.text())
}

pub fn parse_specialization(line: &str) -> Option<Stage> {
    let rest = line.strip_prefix("S ok=1 ")?;
    let (lib, after_lib) = if let Some(lib_rest) = rest.strip_prefix("lib=") {
        let sp = lib_rest.find(' ')?;
        if sp == 0 {
            return None;
        }
        (Some(&lib_rest[..sp]), &lib_rest[sp + 1..])
    } else {
        (None, rest)
    };
    let name_rest = after_lib.strip_prefix("name=")?;
    let sp = name_rest.find(' ')?;
    if sp == 0 {
        return None;
    }
    let name = &name_rest[..sp];
    let after_name = &name_rest[sp + 1..];
    let k_rest = after_name.strip_prefix("k=")?;
    let k_val = match k_rest.find(" | ") {
        Some(pos) => &k_rest[..pos],
        None => k_rest,
    };
    let mut combined = String::from(name);
    if let Some(lib_val) = lib {
        combined.push_str(" lib=");
        combined.push_str(lib_val);
    }
    combined.push(' ');
    combined.push_str(k_val);
    let mut stage = parse_stage(&combined)?;
    stage.spec = true;
    Some(stage)
}
