//! `content_blocking`: the engine's own ad and tracker blocking.

use std::collections::BTreeMap;
use std::path::Path;

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

use crate::{is_site, ConfigError};

/// The engine blocks ads and trackers itself, with Brave's filter engine,
/// in every browser profile. Leave it out to block nothing. See
/// `docs/architecture/CONTENT-BLOCKING.md`.
// `PartialEq` lets a reload detect a change; see the compositor's
// `Restatement`.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize, Serialize, JsonSchema)]
#[serde(default, deny_unknown_fields)]
pub struct ContentBlockingConfig {
    /// Filter lists in Adblock Plus and uBlock Origin syntax, each an
    /// `https://` or `http://` address or an absolute path. The engine
    /// fetches an address again when its list expires (`! Expires:`), after
    /// a day if it names no time. The default is EasyList, EasyPrivacy and
    /// uBlock Origin's filters, privacy, unbreak and badware lists.
    pub lists: Vec<String>,
    /// The scriptlets and stub resources that `##+js()` and `$redirect`
    /// rules name, as adblock-rust's JSON: an address or an absolute path.
    /// The default is Brave's `adblock-resources`.
    pub resources: String,
    /// Your own rules, in the same syntax. They apply in every mode.
    pub filters: Vec<String>,
    /// How much a site's own content is blocked.
    pub mode: BlockingMode,
    /// A mode per site, by host or `*.host` for it and its subdomains. A
    /// site is the address of the page you see, so its frames follow it.
    /// The most specific match wins.
    pub sites: BTreeMap<String, SiteBlocking>,
    /// Also blocks in Electron apps' pages. Off by default: an app can break
    /// in ways no menu fixes.
    pub electron_apps: bool,
}

/// [`ContentBlockingConfig::mode`].
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Deserialize, Serialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum BlockingMode {
    /// Blocks other sites' ads and trackers, and a site's own only when a
    /// rule is `$important`.
    #[default]
    Standard,
    /// Also blocks a site's own ads and trackers.
    Aggressive,
}

/// One site's [`ContentBlockingConfig::sites`] entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize, Serialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum SiteBlocking {
    /// Blocks nothing on the site.
    Off,
    /// [`BlockingMode::Standard`].
    Standard,
    /// [`BlockingMode::Aggressive`].
    Aggressive,
}

const DEFAULT_LISTS: [&str; 6] = [
    "https://easylist.to/easylist/easylist.txt",
    "https://easylist.to/easylist/easyprivacy.txt",
    "https://ublockorigin.github.io/uAssets/filters/filters.txt",
    "https://ublockorigin.github.io/uAssets/filters/privacy.txt",
    "https://ublockorigin.github.io/uAssets/filters/unbreak.txt",
    "https://ublockorigin.github.io/uAssets/filters/badware.txt",
];

const DEFAULT_RESOURCES: &str =
    "https://raw.githubusercontent.com/brave/adblock-resources/master/dist/resources.json";

impl Default for ContentBlockingConfig {
    fn default() -> Self {
        ContentBlockingConfig {
            lists: DEFAULT_LISTS.iter().map(ToString::to_string).collect(),
            resources: DEFAULT_RESOURCES.to_string(),
            filters: Vec::new(),
            mode: BlockingMode::default(),
            sites: BTreeMap::new(),
            electron_apps: false,
        }
    }
}

/// Whether `source` is a list the engine can read: an http or https
/// address, or an absolute path.
fn is_source(source: &str) -> bool {
    source.starts_with("https://")
        || source.starts_with("http://")
        || Path::new(source).is_absolute()
}

impl ContentBlockingConfig {
    pub(crate) fn validate(&self) -> Result<(), ConfigError> {
        let invalid = |message: String| Err(ConfigError::Validation(message));
        if let Some(list) = self.lists.iter().find(|list| !is_source(list)) {
            return invalid(format!(
                "content_blocking.lists holds {list:?}, which is neither an http or https \
                 address nor an absolute path"
            ));
        }
        if !is_source(&self.resources) {
            return invalid(format!(
                "content_blocking.resources {:?} is neither an http or https address nor an \
                 absolute path",
                self.resources
            ));
        }
        if let Some(site) = self.sites.keys().find(|site| !is_site(site)) {
            return invalid(format!(
                "content_blocking.sites {site:?} is not a site such as `github.com` or \
                 `*.github.com`"
            ));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::{BlockingMode, ContentBlockingConfig, SiteBlocking};
    use crate::Config;

    #[test]
    fn no_section_blocks_nothing() {
        assert_eq!(Config::parse("{}").unwrap().content_blocking, None);
    }

    #[test]
    fn an_empty_section_blocks_with_the_default_lists() {
        let config = Config::parse(r#"{ "content_blocking": {} }"#).unwrap();
        assert_eq!(
            config.content_blocking,
            Some(ContentBlockingConfig::default())
        );
        let blocking = config.content_blocking.unwrap();
        assert_eq!(blocking.lists.len(), 6);
        assert_eq!(blocking.mode, BlockingMode::Standard);
    }

    #[test]
    fn sites_and_mode_are_read() {
        let config = Config::parse(
            r#"{ "content_blocking": {
                "lists": ["/etc/domicile/list.txt"],
                "mode": "aggressive",
                "sites": { "bank.example": "off", "*.news.example": "standard" }
            } }"#,
        )
        .unwrap()
        .content_blocking
        .unwrap();
        assert_eq!(config.mode, BlockingMode::Aggressive);
        assert_eq!(config.sites["bank.example"], SiteBlocking::Off);
        assert_eq!(config.sites["*.news.example"], SiteBlocking::Standard);
    }

    #[test]
    fn a_relative_list_is_refused() {
        let error =
            Config::parse(r#"{ "content_blocking": { "lists": ["list.txt"] } }"#).unwrap_err();
        assert!(
            error.to_string().contains("content_blocking.lists"),
            "{error}"
        );
    }

    #[test]
    fn relative_resources_are_refused() {
        let error = Config::parse(r#"{ "content_blocking": { "resources": "resources.json" } }"#)
            .unwrap_err();
        assert!(
            error.to_string().contains("content_blocking.resources"),
            "{error}"
        );
    }

    #[test]
    fn a_site_with_a_path_is_refused() {
        let error =
            Config::parse(r#"{ "content_blocking": { "sites": { "github.com/acme": "off" } } }"#)
                .unwrap_err();
        assert!(
            error.to_string().contains("content_blocking.sites"),
            "{error}"
        );
    }
}
