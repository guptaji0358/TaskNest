import pytest

import repo


def test_finds_root_from_nested_directory(fake_repo):
    nested = fake_repo / "a" / "b"
    nested.mkdir(parents=True)
    assert repo.find_root(nested) == fake_repo.resolve()


def test_finds_root_from_a_file_path(fake_repo):
    target = fake_repo / "notes.txt"
    target.write_text("x", encoding="utf-8")
    assert repo.find_root(target) == fake_repo.resolve()


def test_root_itself_is_accepted(fake_repo):
    assert repo.find_root(fake_repo) == fake_repo.resolve()


def test_missing_start_path_is_rejected(tmp_path):
    with pytest.raises(repo.RepoNotFound):
        repo.find_root(tmp_path / "does-not-exist")


def test_directory_without_markers_is_rejected(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(repo.RepoNotFound):
        repo.find_root(empty)


@pytest.mark.parametrize("missing", ["CMakeLists.txt", ".claude/CLAUDE.md"])
def test_both_markers_are_required(fake_repo, missing):
    (fake_repo / missing).unlink()
    assert not repo.is_root(fake_repo)
    with pytest.raises(repo.RepoNotFound):
        repo.find_root(fake_repo)


def test_marker_must_be_a_file_not_a_directory(fake_repo):
    (fake_repo / "CMakeLists.txt").unlink()
    (fake_repo / "CMakeLists.txt").mkdir()
    assert not repo.is_root(fake_repo)


def test_ctx_paths_and_out_dir_are_not_created(fake_repo):
    ctx = repo.Ctx(root=fake_repo, milestone="M2.0")
    assert ctx.out_dir == fake_repo / "out" / "verify"
    assert ctx.path("api", "openapi.yaml") == fake_repo / "api" / "openapi.yaml"
    assert not ctx.out_dir.exists()
