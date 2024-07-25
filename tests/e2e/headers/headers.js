addEventListener('fetch', evt => evt.respondWith((async () => {
    const headers = new Headers();
    headers.append('Set-cookie', 'A');
    headers.append('set-cookie', 'B');
    console.log([...headers.entries()]);
    console.log('FINE');
    return new Response('test', { headers });
})()));
