// The shell guard-extension-page.sh drives: three pages side by side, in the
// default, the work and the private browser profile.
//
// WHAT THIS PAGE SAYS, all of it to the console, which the engine writes to its
// own log:
//
//   GUARD shell-loaded             this module ran
//   GUARD focused at=…             a page took focus, and the tray its profile
//   GUARD key at=… command=…       a page's `domicile-guest-keydown`, with the
//                                  command of the `?expect=` extension it
//                                  names, or `none`
//   GUARD badge at=… text=…        the extension's badge in that profile, which
//                                  says what its service worker heard
//   GUARD menu at=… items=…        a page's context menu, and the labels of the
//                                  items extensions add to it
//
// It runs each command a key names, and the first item of a menu.
//
// Everything is inside `HostShell`, which the document Domicile writes calls once
// the module has loaded.

export const HostShell = (_root, desktop) => {
  const say = (what) => {
    console.log(`GUARD ${what}`);
  };

  const host = desktop;
  if (host === null || host === undefined) {
    throw new Error(
      "guard-extension-page: no desktop was handed to Shell, so this" +
        " document was not served by the forked engine",
    );
  }

  const params = new URLSearchParams(location.search);
  const expected = params.get("expect");
  const src = params.get("src");
  if (expected === null || src === null) {
    throw new Error("guard-extension-page: ?expect= and ?src= are required");
  }

  host.addEventListener("extensionschanged", () => {
    const action = host.extensions?.find(({ id }) => id === expected);
    if (action !== undefined) {
      say(`badge at=${action.profile} text=${action.badgeText}`);
    }
  });

  const pages = [
    { at: "default", place: (_view) => undefined },
    {
      at: "work",
      place: (view) => {
        view.setAttribute("profile", "work");
      },
    },
    {
      at: "private",
      place: (view) => {
        view.setAttribute("private", "");
      },
    },
  ];
  document.body.style.margin = "0";
  for (const [column, { at, place }] of pages.entries()) {
    const view = document.createElement("webview");
    view.style.position = "absolute";
    view.style.insetBlockStart = "0";
    view.style.insetInlineStart = `${(column * 100) / pages.length}%`;
    view.style.inlineSize = `${100 / pages.length}%`;
    view.style.blockSize = "100%";
    view.style.border = "0";
    place(view);
    view.addEventListener("domicile-guest-focus", () => {
      view.focus();
      host.setExtensionsProfile(at);
      say(`focused at=${at}`);
    });
    view.addEventListener("domicile-guest-keydown", (event) => {
      const { extensionCommand, extensionId } = event;
      const bound = extensionId === expected ? extensionCommand : "none";
      say(`key at=${at} command=${bound}`);
      if (extensionId !== null) {
        host.runExtensionCommand(extensionId, extensionCommand);
      }
    });
    view.addEventListener("domicile-context-menu", (event) => {
      const items = JSON.parse(event.extensionItems);
      say(`menu at=${at} items=${items.map(({ label }) => label).join(",")}.`);
      if (items.length > 0) {
        event.runExtensionItem(items[0].id);
      }
    });
    document.body.append(view);
    // After attaching: it asks for a guest, which needs a frame.
    view.setAttribute("src", `${src}?at=${at}`);
  }
  say("shell-loaded");
};
