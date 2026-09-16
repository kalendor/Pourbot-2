async function loadInstaller() {
  const status = document.querySelector('#version');
  try {
    const response = await fetch('firmware/manifest.json', {cache: 'no-store'});
    if (!response.ok) throw new Error('Release not available');
    const manifest = await response.json();
    const installer = document.querySelector('#installer');
    installer.setAttribute('manifest', `firmware/manifest.json?v=${encodeURIComponent(manifest.version)}`);
    installer.hidden = false;
    status.textContent = `Latest published firmware · v${manifest.version}`;
  } catch (error) {
    status.textContent = 'The installer is being prepared. Please reload shortly or visit GitHub Releases.';
  }
}
loadInstaller();
