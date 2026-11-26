// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

//! The engine's way into adblock-rust. Each call takes and returns plain
//! values, and none throws: a list that is not UTF-8 adds nothing, and
//! resources that do not parse leave the engine's resources as they were.

use std::collections::HashSet;

use adblock::lists::{ExpiresInterval, FilterSet as InnerFilterSet, ParseOptions};
use adblock::request::Request;
use adblock::resources::Resource;
use adblock::url_parser::ResolvesDomain;
use cxx::{let_cxx_string, CxxString, CxxVector};

#[cxx::bridge(namespace = "domicile::content_blocking::rust")]
mod ffi {
    struct DomainPosition {
        start: u32,
        end: u32,
    }

    /// One request's verdict. `redirect` is a `data:` URL and
    /// `rewritten_url` the URL without its removed parameters; each is empty
    /// when there is none.
    struct Verdict {
        matched: bool,
        important: bool,
        exception: bool,
        redirect: String,
        rewritten_url: String,
    }

    extern "Rust" {
        type FilterSet;
        type Engine;

        fn new_filter_set() -> Box<FilterSet>;
        fn add_list(self: &mut FilterSet, text: &[u8]) -> bool;

        fn engine_from_filter_set(set: Box<FilterSet>) -> Box<Engine>;
        fn new_engine() -> Box<Engine>;

        fn install_domain_resolver() -> bool;
        fn list_expires_hours(text: &[u8]) -> u32;

        fn check(
            self: &Engine,
            url: &str,
            host: &str,
            source_host: &str,
            request_type: &str,
            third_party: bool,
            method: &str,
            previously_matched: bool,
            force_exceptions: bool,
        ) -> Verdict;
        fn csp_directives(
            self: &Engine,
            url: &str,
            host: &str,
            source_host: &str,
            request_type: &str,
            third_party: bool,
            method: &str,
        ) -> String;

        fn serialize(self: &Engine) -> Vec<u8>;
        fn deserialize(self: &mut Engine, data: &[u8]) -> bool;
        fn use_resources(self: &mut Engine, json: &str) -> bool;

        fn cosmetic_resources(self: &Engine, url: &str) -> String;
        fn hidden_class_id_selectors(
            self: &Engine,
            classes: &CxxVector<CxxString>,
            ids: &CxxVector<CxxString>,
            exceptions: &CxxVector<CxxString>,
        ) -> Vec<String>;
    }

    unsafe extern "C++" {
        include!("components/domicile/content_blocking/domain_resolver.h");

        fn resolve_domain_position(host: &CxxString) -> DomainPosition;
    }
}

use ffi::{DomainPosition, Verdict};

pub struct FilterSet(InnerFilterSet);

pub struct Engine(adblock::Engine);

fn new_filter_set() -> Box<FilterSet> {
    Box::new(FilterSet(InnerFilterSet::new(false)))
}

impl FilterSet {
    fn add_list(&mut self, text: &[u8]) -> bool {
        match std::str::from_utf8(text) {
            Ok(text) => {
                self.0.add_filter_list(text.to_string(), ParseOptions::default());
                true
            }
            Err(_) => false,
        }
    }
}

fn engine_from_filter_set(set: Box<FilterSet>) -> Box<Engine> {
    Box::new(Engine(adblock::Engine::new_with_filter_set(set.0)))
}

fn new_engine() -> Box<Engine> {
    Box::new(Engine(adblock::Engine::default()))
}

struct Resolver;

impl ResolvesDomain for Resolver {
    fn get_host_domain(&self, host: &str) -> (usize, usize) {
        let_cxx_string!(host = host);
        let DomainPosition { start, end } = ffi::resolve_domain_position(&host);
        (start as usize, end as usize)
    }
}

/// adblock-rust keeps one resolver for the process; a second call is
/// refused, and returns false.
fn install_domain_resolver() -> bool {
    adblock::url_parser::set_domain_resolver(Box::new(Resolver)).is_ok()
}

/// The list's `! Expires:` in hours, or 0 when it names none.
fn list_expires_hours(text: &[u8]) -> u32 {
    let Ok(text) = std::str::from_utf8(text) else {
        return 0;
    };
    match adblock::lists::read_list_metadata(text).expires {
        Some(ExpiresInterval::Hours(hours)) => hours.into(),
        Some(ExpiresInterval::Days(days)) => u32::from(days) * 24,
        None => 0,
    }
}

fn request(
    url: &str,
    host: &str,
    source_host: &str,
    request_type: &str,
    third_party: bool,
    method: &str,
) -> Request {
    Request::preparsed(url, host, source_host, request_type, third_party, method)
}

impl Engine {
    #[allow(clippy::too_many_arguments)]
    fn check(
        &self,
        url: &str,
        host: &str,
        source_host: &str,
        request_type: &str,
        third_party: bool,
        method: &str,
        previously_matched: bool,
        force_exceptions: bool,
    ) -> Verdict {
        let result = self.0.check_network_request_subset(
            &request(url, host, source_host, request_type, third_party, method),
            previously_matched,
            force_exceptions,
        );
        Verdict {
            matched: result.filter.is_some(),
            important: result.important,
            exception: result.exception.is_some(),
            redirect: result.redirect.unwrap_or_default(),
            rewritten_url: result.rewritten_url.unwrap_or_default(),
        }
    }

    fn csp_directives(
        &self,
        url: &str,
        host: &str,
        source_host: &str,
        request_type: &str,
        third_party: bool,
        method: &str,
    ) -> String {
        self.0
            .get_csp_directives(&request(url, host, source_host, request_type, third_party, method))
            .unwrap_or_default()
    }

    /// Empty when the engine cannot be written.
    fn serialize(&self) -> Vec<u8> {
        self.0.serialize()
    }

    fn deserialize(&mut self, data: &[u8]) -> bool {
        self.0.deserialize(data).is_ok()
    }

    /// `json` is adblock-rust's resource list, as Brave's
    /// adblock-resources builds it.
    fn use_resources(&mut self, json: &str) -> bool {
        match serde_json::from_str::<Vec<Resource>>(json) {
            Ok(resources) => {
                self.0.use_resources(resources);
                true
            }
            Err(_) => false,
        }
    }

    /// adblock-rust's `UrlSpecificResources` for `url`, as JSON.
    fn cosmetic_resources(&self, url: &str) -> String {
        serde_json::to_string(&self.0.url_cosmetic_resources(url)).unwrap_or_default()
    }

    fn hidden_class_id_selectors(
        &self,
        classes: &CxxVector<CxxString>,
        ids: &CxxVector<CxxString>,
        exceptions: &CxxVector<CxxString>,
    ) -> Vec<String> {
        let texts = |values: &CxxVector<CxxString>| -> Vec<String> {
            values.iter().filter_map(|value| value.to_str().ok().map(str::to_string)).collect()
        };
        let exceptions: HashSet<String> = texts(exceptions).into_iter().collect();
        self.0.hidden_class_id_selectors(texts(classes), texts(ids), &exceptions)
    }
}
