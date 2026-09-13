#include "photobridge/lan/upload_page.h"

namespace photobridge {

UploadPageResponse GetUploadPageResponse()
{
    return UploadPageResponse{
        "text/html; charset=utf-8",
        R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>PhotoBridge Upload</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, sans-serif; }
    body { max-width: 42rem; margin: 3rem auto; padding: 0 1rem; }
    main { display: grid; gap: 1rem; }
    button { min-height: 2.75rem; padding: 0 1rem; }
    progress { width: 100%; }
    #status { min-height: 1.5rem; }
  </style>
</head>
<body>
  <main>
    <h1>PhotoBridge Upload</h1>
    <p>Select photos or videos to upload to this PhotoBridge session.</p>
    <label for="files">Files</label>
    <input id="files" type="file" multiple>
    <button id="upload" type="button">Upload selected files</button>
    <progress id="progress" max="1" value="0" hidden></progress>
    <p id="status" role="status" aria-live="polite"></p>
  </main>
  <script>
    (() => {
      const files = document.querySelector('#files');
      const button = document.querySelector('#upload');
      const progress = document.querySelector('#progress');
      const status = document.querySelector('#status');
      const token = new URLSearchParams(location.search).get('t');
      const headers = () => ({
        'Authorization': `Bearer ${token || ''}`,
        'Cache-Control': 'no-store'
      });
      const show = text => { status.textContent = text; };
      const readJson = async response => {
        if (!response.ok) throw new Error(await response.text());
        return response.json();
      };
      const uploadOne = async (sessionId, file, fileId) => {
        for (let attempt = 1; attempt <= 3; ++attempt) {
          try {
            const response = await fetch(
              `/api/v1/sessions/${encodeURIComponent(sessionId)}/files/${encodeURIComponent(fileId)}`,
              { method: 'PUT', headers: { ...headers(), 'Content-Type': 'application/octet-stream' }, body: file });
            if (response.ok) return;
            if (response.status === 409 && attempt < 3) continue;
            throw new Error(await response.text());
          } catch (error) {
            if (attempt === 3) throw error;
          }
        }
      };
      const runThreeLane = async (jobs, upload) => {
        const largeThreshold = 256 * 1024 * 1024;
        const large = jobs.filter(job => job.file.size >= largeThreshold);
        const small = jobs.filter(job => job.file.size < largeThreshold);
        let activeLarge = 0;
        const take = lane => {
          if (lane === 0 && large.length > 0 && activeLarge < 2) {
            ++activeLarge;
            return large.shift();
          }
          if (small.length > 0) return small.shift();
          if (large.length > 0 && activeLarge < 2) {
            ++activeLarge;
            return large.shift();
          }
          return null;
        };
        const worker = async lane => {
          while (true) {
            const job = take(lane);
            if (job === null) return;
            try { await upload(job); }
            finally {
              if (job.file.size >= largeThreshold) --activeLarge;
            }
          }
        };
        await Promise.all([worker(0), worker(1), worker(2)]);
      };
      button.addEventListener('click', async () => {
        const selected = [...files.files];
        if (selected.length === 0) { show('Choose at least one file.'); return; }
        button.disabled = true;
        progress.hidden = false;
        progress.max = selected.length;
        progress.value = 0;
        try {
          const session = await readJson(await fetch('/api/v1/sessions', {
            method: 'POST', headers: { ...headers(), 'Content-Type': 'application/json' },
            body: JSON.stringify({ files: selected.map(file => ({
              original_filename: file.name, file_size: file.size })) })
          }));
          let completed = 0;
          await runThreeLane(
            selected.map((file, index) => ({
              file,
              fileId: session.files[index].file_id,
            })),
            async job => {
              show(`Uploading ${completed + 1}/${selected.length}: ${job.file.name}`);
              await uploadOne(session.session_id, job.file, job.fileId);
              progress.value = ++completed;
            });
          await readJson(await fetch(
            `/api/v1/sessions/${encodeURIComponent(session.session_id)}/complete`,
            { method: 'POST', headers: headers() }));
          show(`Completed ${selected.length} file(s).`);
        } catch (error) {
          show(`Upload failed: ${error.message}`);
        } finally {
          button.disabled = false;
        }
      });
    })();
  </script>
</body>
</html>
)HTML",
    };
}

bool ConstantTimeTokenEquals(
    std::string_view provided,
    std::string_view expected) noexcept
{
    const std::size_t length =
        provided.size() > expected.size() ? provided.size() : expected.size();
    unsigned char difference = static_cast<unsigned char>(
        provided.size() ^ expected.size());
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char left = index < provided.size()
            ? static_cast<unsigned char>(provided[index])
            : 0;
        const unsigned char right = index < expected.size()
            ? static_cast<unsigned char>(expected[index])
            : 0;
        difference = static_cast<unsigned char>(difference | (left ^ right));
    }
    return difference == 0;
}

}  // namespace photobridge
