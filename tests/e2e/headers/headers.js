import { strictEqual, deepStrictEqual, throws } from "../../assert.js";

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

      var headerSeqCombine = [["single", "singleValue"],
      ["double", "doubleValue1"],
      ["double", "doubleValue2"],
      ["triple", "tripleValue1"],
      ["triple", "tripleValue2"],
      ["triple", "tripleValue3"]
      ];
      var expectedDict = {"single": "singleValue",
        "double": "doubleValue1, doubleValue2",
        "triple": "tripleValue1, tripleValue2, tripleValue3"
      };
      var headers = new Headers(headerSeqCombine);
      for (const name in expectedDict) {
        var value = headers.get(name);
        headers.append(name,"newSingleValue");
        strictEqual(headers.get(name), (value + ", " + "newSingleValue"));
      }

      response.headers.append("Set-cookie", "A");
      response.headers.append("Another", "A");
      response.headers.append("set-cookie", "B");
      response.headers.append("another", "B");

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
