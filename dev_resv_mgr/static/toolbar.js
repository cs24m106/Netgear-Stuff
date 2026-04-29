/**
 * Shared toolbar: theme (localStorage) for Manager + Statistics pages.
 */
(function () {
    const THEME_KEY = 'switchmgr.theme';

    function setTheme(theme) {
        if (theme === 'dark') document.body.setAttribute('data-theme', 'dark');
        else document.body.removeAttribute('data-theme');
        try {
            localStorage.setItem(THEME_KEY, theme);
        } catch (e) {}
    }

    function toggleTheme() {
        const cur = document.body.getAttribute('data-theme') === 'dark' ? 'dark' : 'light';
        const next = cur === 'dark' ? 'light' : 'dark';
        setTheme(next);
    }

    function initThemeControls() {
        try {
            const saved = localStorage.getItem(THEME_KEY) || 'light';
            setTheme(saved);
        } catch (e) {}
        const btn = document.getElementById('themeToggle');
        if (!btn) return;
        btn.addEventListener('click', toggleTheme);
        btn.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                toggleTheme();
            }
        });
    }

    window.SwitchMgrTheme = { setTheme, toggleTheme, THEME_KEY };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initThemeControls);
    } else {
        initThemeControls();
    }
})();
