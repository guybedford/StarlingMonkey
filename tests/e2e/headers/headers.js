import { strictEqual, deepStrictEqual, throws } from "../../assert.js";

var log = [];
function clearLog() {
  log = [];
}
function addLogEntry(name, args) {
  log.push([ name, ...args ]);
}

var loggingHandler = {
};

addEventListener("fetch", (evt) =>
  evt.respondWith(
    (async () => {
      strictEqual(evt.request.headers.get("EXAMPLE-HEADER"), "Header Value");
      throws(
        () => {
          evt.request.headers.delete("user-agent");
        },
        TypeError,
        "Headers.delete: Headers are immutable"
      );
      const response = new Response("test", {
        headers: [...evt.request.headers.entries()].filter(
          ([name]) => name !== "content-type" && name !== 'content-length'
        ),
      });

      for (let prop of Object.getOwnPropertyNames(Reflect)) {
        loggingHandler[prop] = function(...args) {
          addLogEntry(prop, args);
          return Reflect[prop](...args);
        }
      }
      var record = {};
      Object.defineProperty(record, "a", { value: "b", enumerable: false });
      Object.defineProperty(record, "c", { value: "d", enumerable: true });
      Object.defineProperty(record, "e", { value: "f", enumerable: false });
      console.log(Object.getOwnPropertyDescriptor(record, 'a'));
      var proxy = new Proxy(record, loggingHandler);
      var h = new Headers(proxy);

      console.log([...h]);
    
      // strictEqual(log.length, 6);
      // The first thing is the [[Get]] of Symbol.iterator to figure out whether
      // we're a sequence, during overload resolution.
      deepStrictEqual(log[0], ["get", record, Symbol.iterator, proxy]);
      // Then we have the [[OwnPropertyKeys]] from
      // https://webidl.spec.whatwg.org/#es-to-record step 4.
      deepStrictEqual(log[1], ["ownKeys", record]);
      // Then the [[GetOwnProperty]] from step 5.1.
      deepStrictEqual(log[2], ["getOwnPropertyDescriptor", record, "a"]);
      // No [[Get]] because not enumerable
      // Then the second [[GetOwnProperty]] from step 5.1.
      deepStrictEqual(log[3], ["getOwnPropertyDescriptor", record, "c"]);
      // Then the [[Get]] from step 5.2.
      deepStrictEqual(log[4], ["get", record, "c", proxy]);
      // Then the third [[GetOwnProperty]] from step 5.1.
      deepStrictEqual(log[5], ["getOwnPropertyDescriptor", record, "e"]);
      // No [[Get]] because not enumerable
    
      // Check the results.
      strictEqual([...h].length, 1);
      deepStrictEqual([...h.keys()], ["c"]);
      strictEqual(h.has("c"), true);
      strictEqual(h.get("c"), "d");

      response.headers.append("Set-cookie", "A");
      response.headers.append("Another", "A");
      response.headers.append("set-cookie", "B");
      response.headers.append("another", "B");
      response.headers.append("newLine", "newLine\xa0");
      response.headers.get("newLine");
      deepStrictEqual(
        [...response.headers],
        [
          ["accept", "*/*"],
          ["another", "A, B"],
          ["content-length", "4"],
          ["content-type", "text/plain;charset=UTF-8"],
          ["example-header", "Header Value"],
          ["set-cookie", "A"],
          ["set-cookie", "B"],
          ["user-agent", "test-agent"],
        ]
      );
      response.headers.delete("accept");
      return response;
    })()
  )
);
