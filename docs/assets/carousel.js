// Simple YouTube video carousel for MkDocs Material

document.addEventListener('DOMContentLoaded', function () {
  var carousel = document.getElementById('yt-carousel');
  if (!carousel) return;
  // Support both data-ids (comma-separated) and data-videos (JSON array of {id, title})
  var videos = [];
  if (carousel.dataset.videos) {
    try {
      videos = JSON.parse(carousel.dataset.videos);
    } catch (e) {
      console.error('Invalid JSON in data-videos:', e);
    }
  } else if (carousel.dataset.ids) {
    videos = carousel.dataset.ids.split(',').map(function(id) {
      return { id: id.trim(), title: '' };
    });
  }
  if (!videos.length) return;
  var idx = 0;

  function render() {
    var video = videos[idx];
    var titleHtml = video.title ? `<div style=\"margin-bottom:8px;font-weight:bold;font-size:1.1rem;\">${video.title}</div>` : '';
    carousel.innerHTML = `
      <div style="display:flex;flex-direction:column;align-items:center;max-width:100%;">
        ${titleHtml}
        <div style="position:relative;width:100%;max-width:560px;aspect-ratio:16/9;">
          <iframe style="width:100%;height:100%;border-radius:12px;box-shadow:0 2px 16px #0003;" src="https://www.youtube.com/embed/${video.id}" frameborder="0" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture" allowfullscreen></iframe>
        </div>
        <div style="margin-top:16px;display:flex;align-items:center;gap:16px;">
          <button id="yt-prev" style="padding:2px 8px;font-size:0.6rem;border:none;border-radius:6px;background:#E59800;color:#222;font-weight:bold;cursor:pointer;">Prev</button>
          <span style="font-size:1rem;color:#666;">${idx+1} / ${videos.length}</span>
          <button id="yt-next" style="padding:2px 8px;font-size:0.6rem;border:none;border-radius:6px;background:#E59800;color:#222;font-weight:bold;cursor:pointer;">Next</button>
        </div>
      </div>
    `;
    document.getElementById('yt-prev').onclick = function() {
      idx = (idx - 1 + videos.length) % videos.length;
      render();
    };
    document.getElementById('yt-next').onclick = function() {
      idx = (idx + 1) % videos.length;
      render();
    };
  }
  render();
});
