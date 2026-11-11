from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    b=p.chromium.launch()
    pg=b.new_page()
    pg.goto('file:///tmp/claude-0/guia/guia_final.html'); pg.wait_for_timeout(800)
    pg.pdf(path='/tmp/claude-0/guia/Guia de Gaux Equalizer.pdf', format='A4', print_background=True, prefer_css_page_size=True)
    b.close()
