addEventListener('fetch', evt => evt.respondWith((async () => {
    const headers = new Headers();
    headers.append('Set-cookie', 'A');
    headers.append('set-cookie', 'B');
    return new Response('test', { headers });
})()));
