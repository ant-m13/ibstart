"""Point actions on the generated changelog page to its real source file."""


def on_pre_page(page, config, files):
    if page.file.src_uri == "changelog.md":
        page.edit_url = f"{config['repo_url'].rstrip('/')}/edit/main/CHANGELOG.md"
