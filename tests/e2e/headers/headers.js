import { strictEqual, deepStrictEqual } from "../../assert.js";

addEventListener("fetch", (evt) =>
  evt.respondWith(
    (async () => {
      strictEqual(evt.request.headers.get("EXAMPLE-HEADER"), "Header Value");
      // TODO: this should throw immutable?
      // evt.request.headers.delete("user-agent");
      const headers = new Headers(evt.request.headers);
      headers.append("Set-cookie", "A");
      headers.append("Another", "A");
      headers.append("set-cookie", "B");
      headers.append("another", "B");
      deepStrictEqual(
        [...headers],
        [
          ["accept", "*/*"],
          ["another", "A, B"],
          ["example-header", "Header Value"],
          ["set-cookie", "A"],
          ["set-cookie", "B"],
          ["user-agent", "test-agent"]
        ]
      );
      headers.delete('accept');
      return new Response("test", { headers });
    })()
  )
);
