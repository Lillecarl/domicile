import { describe, expect, it } from "bun:test";
import {
  extensionCommandFor,
  extensionCommandsSchema,
  extensionPopupSchema,
} from "./extension";

const ID = "abcdefghijklmnopabcdefghijklmnop";

describe("extensionPopupSchema", () => {
  it("refuses a popup outside an extension", () => {
    expect(
      extensionPopupSchema.safeParse({ id: ID, popup: "https://example.com/" })
        .success,
    ).toBe(false);
  });
});

describe(extensionCommandFor, () => {
  const extensions = [
    extensionCommandsSchema.parse({
      commands: [
        { description: "", global: false, keys: "", name: "unbound" },
        { description: "", global: false, keys: "Ctrl+Shift+L", name: "fill" },
      ],
      id: ID,
      name: "Bitwarden",
      profile: "default",
    }),
  ];
  const press = {
    altKey: false,
    code: "KeyL",
    ctrlKey: true,
    key: "L",
    metaKey: false,
    shiftKey: true,
  };

  it("names the command the keys are bound to", () => {
    expect(extensionCommandFor(extensions, press)).toStrictEqual({
      id: ID,
      name: "fill",
    });
  });

  it("names none for other keys", () => {
    expect(
      extensionCommandFor(extensions, { ...press, shiftKey: false }),
    ).toBeUndefined();
  });
});
