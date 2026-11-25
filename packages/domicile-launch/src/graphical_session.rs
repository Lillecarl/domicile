//! Registers a desktop as the graphical session with the systemd user manager.
//!
//! Services such as the portal require `graphical-session.target`, which
//! only a session can start. [`name`] exports the desktop's name before any
//! component starts, since the engine can D-Bus activate the portal frontend
//! before the compositor is up. [`begin`] exports the rest of the desktop's
//! environment and then starts [`TARGET`], so services started by it inherit
//! that environment.
//! [`end`] stops the session and unsets the environment, so later services do
//! not find a dead socket or display.
//!
//! A second login session, on another tty, takes over the user manager's
//! session from the first, which may be another compositor such as niri.
//! [`name`] returns the first's variables, [`begin`] keeps them, and [`end`]
//! gives them back while the first still runs instead of stopping the session
//! under it.
//!
//! Only a desktop running as the login session does this. A nested desktop
//! leaves the session to its host.

use std::collections::HashMap;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};

use crate::control_socket::VARIABLE;

/// The unit started while the desktop is the session. It has
/// `BindsTo=graphical-session.target`.
pub const TARGET: &str = "domicile-session.target";

/// The unit that ends the session.
///
/// Stopping [`TARGET`] is not enough: a running portal's `Requisite=` keeps
/// `graphical-session.target` up. This unit `Conflicts=` the graphical session,
/// like niri's `niri-shutdown.target`.
pub const SHUTDOWN: &str = "domicile-session-shutdown.target";

/// The portal frontend's unit.
const FRONTEND: &str = "xdg-desktop-portal.service";

/// `XDG_CURRENT_DESKTOP` for a desktop. Matches the names of
/// `domicile-mimeapps.list` and `domicile-portals.conf`.
const CURRENT_DESKTOP: &str = "domicile";

/// Variables [`begin`] sets and [`end`] unsets.
const VARIABLES: [&str; 4] = [
    "XDG_CURRENT_DESKTOP",
    "WAYLAND_DISPLAY",
    VARIABLE,
    "XDG_SESSION_TYPE",
];

/// Exports `XDG_CURRENT_DESKTOP` and `XDG_SESSION_TYPE` to `manager`.
///
/// The portal frontend reads `XDG_CURRENT_DESKTOP` once, when it starts, and
/// routes every interface by it. Neither value waits on the compositor.
///
/// Returns the [`VARIABLES`] `manager` held before, for [`begin`].
pub fn name(manager: &zbus::Connection) -> Result<Vec<String>, zbus::Error> {
    let before = held(manager)?;
    call(
        manager,
        "SetEnvironment",
        &(vec![
            format!("XDG_CURRENT_DESKTOP={CURRENT_DESKTOP}"),
            "XDG_SESSION_TYPE=wayland".to_string(),
        ],),
    )?;
    Ok(before)
}

/// Exports the desktop's environment to `manager`, then starts [`TARGET`].
///
/// Returns `before`, the variables [`name`] replaced, for [`end`]. Empty when
/// they name this desktop's control socket.
pub fn begin(
    manager: &zbus::Connection,
    wayland_display: &str,
    control: &Path,
    before: &[String],
) -> Result<Vec<String>, zbus::Error> {
    let earlier = match socket(before) {
        Some(socket) if Path::new(socket) == control => Vec::new(),
        _ => before.to_vec(),
    };
    let values = [
        CURRENT_DESKTOP.to_string(),
        wayland_display.to_string(),
        control.display().to_string(),
        "wayland".to_string(),
    ];
    let assignments: Vec<String> = VARIABLES
        .iter()
        .zip(values)
        .map(|(name, value)| format!("{name}={value}"))
        .collect();
    call(manager, "SetEnvironment", &(assignments,))?;
    call(manager, "StartUnit", &(TARGET, "replace"))?;
    Ok(earlier)
}

/// Ends the session of the desktop at `control`, which took it over from
/// `earlier` (from [`begin`]).
///
/// - Another desktop's control socket in `manager`: that desktop took the
///   session over later, so it is left alone.
/// - `earlier`'s desktop still answers: its variables are set again, and the
///   [`VARIABLES`] it lacks are unset. When it is another compositor, [`TARGET`]
///   is stopped too, as no Domicile desktop is the session. That leaves a
///   `graphical-session.target` the compositor holds up running. The bus
///   daemon gets the variables and the portal frontend restarts, which a
///   Domicile desktop does itself when it gets the portal backend's name.
/// - Otherwise starts [`SHUTDOWN`], then unsets the environment from
///   [`begin`]. Uses `replace-irreversibly`, as niri does, so no later job can
///   cancel the shutdown.
pub fn end(
    manager: &zbus::Connection,
    control: &Path,
    earlier: &[String],
) -> Result<(), zbus::Error> {
    let held = held(manager)?;
    if socket(&held).is_some_and(|socket| Path::new(socket) != control) {
        return Ok(());
    }
    match running(earlier, control) {
        Some(Running::Domicile) => restore(manager, earlier),
        Some(Running::Other) => {
            restore(manager, earlier)?;
            call(manager, "StopUnit", &(TARGET, "replace"))?;
            activate_in(manager, earlier)?;
            restart_portal_frontend(manager)
        }
        None => {
            call(manager, "StartUnit", &(SHUTDOWN, "replace-irreversibly"))?;
            call(manager, "UnsetEnvironment", &(VARIABLES.to_vec(),))
        }
    }
}

/// Sets `earlier` in `manager`, and unsets the [`VARIABLES`] it lacks.
fn restore(manager: &zbus::Connection, earlier: &[String]) -> Result<(), zbus::Error> {
    let lacking: Vec<&str> = VARIABLES
        .into_iter()
        .filter(|name| value(earlier, name).is_none())
        .collect();
    call(manager, "UnsetAndSetEnvironment", &(lacking, earlier))
}

/// Sets `earlier` in the bus daemon's activation environment, for the services
/// it starts directly. The daemon has no call to unset a variable.
fn activate_in(bus: &zbus::Connection, earlier: &[String]) -> Result<(), zbus::Error> {
    let environment: HashMap<&str, &str> = earlier
        .iter()
        .filter_map(|assignment| assignment.split_once('='))
        .collect();
    zbus::block_on(bus.call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        "UpdateActivationEnvironment",
        &(environment,),
    ))
    .map(drop)
}

/// Restarts the portal frontend if it runs. It reads `XDG_CURRENT_DESKTOP`
/// once, when it starts. A system without the frontend has none to restart.
fn restart_portal_frontend(manager: &zbus::Connection) -> Result<(), zbus::Error> {
    match call(manager, "TryRestartUnit", &(FRONTEND, "replace")) {
        Err(zbus::Error::MethodError(name, ..))
            if name == "org.freedesktop.systemd1.NoSuchUnit" =>
        {
            Ok(())
        }
        restarted => restarted,
    }
}

/// The desktop a session was taken over from, while it still runs.
enum Running {
    Domicile,
    Other,
}

/// What runs of the desktop `earlier` names, if anything.
///
/// A Domicile desktop answers on its control socket, another compositor on its
/// Wayland socket. A relative `WAYLAND_DISPLAY` is in `XDG_RUNTIME_DIR`, which
/// also holds `control` ([`crate::control_socket::address`]), and an absolute
/// one replaces the whole path.
fn running(earlier: &[String], control: &Path) -> Option<Running> {
    let (listening, running) = match (socket(earlier), value(earlier, "WAYLAND_DISPLAY")) {
        (Some(socket), _) => (PathBuf::from(socket), Running::Domicile),
        (None, Some(display)) => (control.with_file_name(display), Running::Other),
        (None, None) => return None,
    };
    UnixStream::connect(listening).is_ok().then_some(running)
}

/// The assignments of [`VARIABLES`] in `manager`'s environment.
fn held(manager: &zbus::Connection) -> Result<Vec<String>, zbus::Error> {
    let environment: Vec<String> = zbus::block_on(
        zbus::block_on(zbus::Proxy::new(
            manager,
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
        ))?
        .get_property("Environment"),
    )?;
    Ok(environment
        .into_iter()
        .filter(|assignment| {
            VARIABLES
                .iter()
                .any(|name| assignment.split_once('=').map(|(key, _)| key) == Some(*name))
        })
        .collect())
}

/// The control socket among `assignments`.
fn socket(assignments: &[String]) -> Option<&str> {
    value(assignments, VARIABLE)
}

/// The value `assignments` gives `name`.
fn value<'a>(assignments: &'a [String], name: &str) -> Option<&'a str> {
    assignments.iter().find_map(|assignment| {
        assignment
            .strip_prefix(name)
            .and_then(|rest| rest.strip_prefix('='))
    })
}

fn call<B>(manager: &zbus::Connection, method: &str, body: &B) -> Result<(), zbus::Error>
where
    B: serde::Serialize + zbus::DynamicType,
{
    zbus::block_on(manager.call_method(
        Some("org.freedesktop.systemd1"),
        "/org/freedesktop/systemd1",
        Some("org.freedesktop.systemd1.Manager"),
        method,
        body,
    ))
    .map(drop)
}
