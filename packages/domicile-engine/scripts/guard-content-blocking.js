// The shell guard-content-blocking.sh drives: one browser window showing the
// page ?src= names. A module, because only a domicile:// document may ask
// for a guest. Once the window counts a blocked request, so the engine has
// built its blocker, it reports what the shell's blocking calls answer.
//
// The document Domicile writes calls `HostShell` once the module loads.

export const HostShell = (_root, desktop) => {
  if (desktop === null || desktop === undefined) {
    throw new Error(
      "guard-content-blocking: no desktop was handed to Shell, so this" +
        " document is not a shell the engine serves",
    );
  }
  const src = new URLSearchParams(location.search).get("src");
  if (src === null) {
    throw new Error("guard-content-blocking: ?src= is required");
  }

  const view = document.createElement("webview");
  view.style.position = "absolute";
  view.style.inset = "0";
  view.style.inlineSize = "100%";
  view.style.blockSize = "100%";
  view.style.border = "0";

  const report = async (counted) => {
    const mode = await desktop.contentBlockingFor(src);
    await desktop.setContentBlockingSite("localhost", "aggressive");
    const set = await desktop.contentBlockingFor("http://localhost/");
    await desktop.setContentBlockingSite("localhost", "");
    const unset = await desktop.contentBlockingFor("http://localhost/");
    const refused = await desktop
      .setContentBlockingSite("not a site", "off")
      .then(
        () => "none",
        (error) => error.name,
      );
    console.log(
      `GUARD shell counted=${counted > 0} mode=${mode} set=${set}` +
        ` unset=${unset} refused=${refused}`,
    );
  };

  let opened = false;
  let reported = false;
  desktop.addEventListener("browserwindowschanged", () => {
    const windows = desktop.browserWindows ?? [];
    if (!opened && windows.length > 0) {
      opened = true;
      view.setAttribute("window", windows[0].id);
      document.body.append(view);
    }
    if (!reported && windows.length > 0 && windows[0].blockedRequests > 0) {
      reported = true;
      report(windows[0].blockedRequests).catch((error) => {
        console.log(`GUARD shell failed ${error}`);
      });
    }
  });

  document.body.style.margin = "0";
  desktop.openBrowserWindow(src);
  console.log("GUARD shell-loaded");
};
