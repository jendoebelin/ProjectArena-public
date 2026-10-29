"""Discord posts for the Project Arena server, through channel webhooks.
  preview:  python -I tools/discord/discord_posts.py preview   (writes build/discord_draft/preview.html; sends nothing)
  post:     python -I tools/discord/discord_posts.py post [CHANNEL ...] [--only "TITLE" | --last N]   (posts; the owner
            approves first; --only: just the post with that title, --last: the channel's last N posts in order, not
            every post for the channel)
Posts: tools/discord/posts.json (channel, title, colour, markdown text, optional picture from build/discord_draft).
Webhooks: discord.local.cfg in the project root (gitignored), one line per channel: CHANNEL=https://discord.com/api/webhooks/...
A webhook address lets anyone post in that channel: never print it, never commit it."""
import html
import json
import os
import re
import sys
import urllib.request
import time
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
POSTS = os.path.join(ROOT, 'tools', 'discord', 'posts.json')
PICS = os.path.join(ROOT, 'build', 'discord_draft')
CFG = os.path.join(ROOT, 'discord.local.cfg')


def load_hooks():
    hooks = {}
    for ln in open(CFG, encoding='utf-8-sig'):
        ln = ln.strip()
        if '=' in ln and not ln.startswith(('#', 'http')):
            k, v = ln.split('=', 1)
            hooks[k.strip().lstrip('#')] = v.strip()
    return hooks


def md(text):   # enough Discord markdown for the preview: bold, code, links, line breaks
    t = html.escape(text)
    t = re.sub(r'\*\*(.+?)\*\*', r'<b>\1</b>', t)
    t = re.sub(r'`(.+?)`', r'<code>\1</code>', t)
    t = re.sub(r'(https://[^\s<]+)', r'<a href="\1">\1</a>', t)
    return t.replace('\n', '<br>')


def preview(posts):
    out = ['<!DOCTYPE html><html><head><meta charset="utf-8"><title>Discord draft</title><style>',
           'body{background:#313338;color:#dbdee1;font-family:"gg sans","Segoe UI",sans-serif;margin:0;padding:24px}',
           'h2{color:#949ba4;font-size:13px;letter-spacing:.08em;margin:28px 0 8px}',
           '.msg{display:flex;gap:14px;max-width:720px}.av{width:40px;height:40px;border-radius:50%;background:#0b0f1a;flex:none}',
           '.name{color:#fff;font-weight:600}.bot{background:#5865f2;color:#fff;font-size:10px;border-radius:3px;padding:1px 4px;margin-left:4px}',
           '.embed{background:#2b2d31;border-left:4px solid var(--c);border-radius:4px;padding:10px 16px 14px;margin-top:6px;max-width:520px}',
           '.t{color:#fff;font-weight:700;margin-bottom:6px}.d{font-size:14.5px;line-height:1.38}',
           'img{max-width:100%;border-radius:4px;margin-top:12px}a{color:#00a8fc;text-decoration:none}',
           'code{background:#1e1f22;padding:1px 4px;border-radius:3px;font-size:13px}</style></head><body>',
           '<p style="color:#949ba4">Draft only: nothing has been posted. Each block is one message in the channel named above it.</p>']
    for p in posts:
        img = '<img src="%s">' % p['image'] if p.get('image') else ''
        out.append('<h2># %s</h2><div class="msg"><img class="av" src="../../server/crowd_web/icon-192.png" style="margin:0">'
                   '<div><span class="name">Project Arena</span><span class="bot">APP</span>'
                   '<div class="embed" style="--c:%s"><div class="t">%s</div><div class="d">%s</div>%s</div></div></div>'
                   % (html.escape(p['channel']), p['color'], html.escape(p['title']), md(p['text']), img))
    out.append('</body></html>')
    path = os.path.join(PICS, 'preview.html')
    open(path, 'w', encoding='utf-8').write('\n'.join(out))
    print('preview:', path)


def send(url, post):
    embed = {'title': post['title'], 'description': post['text'], 'color': int(post['color'][1:], 16)}
    payload = {'username': 'Project Arena', 'embeds': [embed], 'allowed_mentions': {'parse': []}}
    files = []
    if post.get('image'):
        embed['image'] = {'url': 'attachment://' + post['image']}
        files.append((post['image'], open(os.path.join(PICS, post['image']), 'rb').read()))
    b = uuid.uuid4().hex
    body = b''
    body += ('--%s\r\nContent-Disposition: form-data; name="payload_json"\r\nContent-Type: application/json\r\n\r\n' % b).encode()
    body += json.dumps(payload).encode() + b'\r\n'
    for i, (name, data) in enumerate(files):
        body += ('--%s\r\nContent-Disposition: form-data; name="files[%d]"; filename="%s"\r\nContent-Type: image/png\r\n\r\n'
                 % (b, i, name)).encode() + data + b'\r\n'
    body += ('--%s--\r\n' % b).encode()
    req = urllib.request.Request(url + '?wait=true', data=body, method='POST', headers={
        'Content-Type': 'multipart/form-data; boundary=' + b, 'User-Agent': 'ProjectArena/1.0'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # (titles with emoji, on any console)
    posts = json.load(open(POSTS, encoding='utf-8'))
    if len(sys.argv) < 2 or sys.argv[1] == 'preview':
        preview(posts)
        return
    if sys.argv[1] == 'post':
        hooks = load_hooks()
        args = sys.argv[2:]
        only = args[args.index('--only') + 1] if '--only' in args else None
        last = int(args[args.index('--last') + 1]) if '--last' in args else 0
        want = set(a for i, a in enumerate(args) if not a.startswith('--') and (i == 0 or args[i - 1] not in ('--only', '--last')))
        pick = [p for p in posts if (not want or p['channel'] in want) and (not only or p['title'] == only)]
        if last:
            pick = pick[-last:]
        for k, p in enumerate(pick):
            if k:
                time.sleep(1.5)   # (in order: each one after the last has gone)
            url = hooks.get(p['channel'])
            if not url:
                print('no webhook for #%s (add a line "%s=..." to discord.local.cfg): skipped' % (p['channel'], p['channel']))
                continue
            try:
                print('#%s: %s -> %s' % (p['channel'], p['title'], send(url, p)))
            except Exception as e:   # (never print the webhook address)
                print('#%s: %s failed: %s' % (p['channel'], p['title'], type(e).__name__))


if __name__ == '__main__':
    main()
