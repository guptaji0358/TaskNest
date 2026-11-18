"""Account tables of CLOUD.md 16.6 (M2 baseline). Ids are UUIDs generated in application code.

recovery_codes (M2.2) and every sync table (M3) are deliberately absent.
"""
from __future__ import annotations

import sqlalchemy as sa

NAMING = {
    "ix": "ix_%(column_0_label)s",
    "uq": "uq_%(table_name)s_%(column_0_name)s",
    "ck": "ck_%(table_name)s_%(constraint_name)s",
    "fk": "fk_%(table_name)s_%(column_0_name)s_%(referred_table_name)s",
    "pk": "pk_%(table_name)s",
}
metadata = sa.MetaData(naming_convention=NAMING)

_uuid = sa.Uuid(as_uuid=True)
_ts = sa.DateTime(timezone=True)

users = sa.Table(
    "users", metadata,
    sa.Column("id", _uuid, primary_key=True),
    sa.Column("login", sa.Text, nullable=False),  # normalized (NFKC + casefold) by the caller
    sa.Column("password_hash", sa.Text, nullable=False),
    sa.Column("created_at", _ts, nullable=False, server_default=sa.func.now()),
    sa.UniqueConstraint("login", name="uq_users_login"),
)

devices = sa.Table(
    "devices", metadata,
    sa.Column("id", _uuid, primary_key=True),
    sa.Column("account_id", _uuid, sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
    sa.Column("name", sa.Text, nullable=False),
    sa.Column("platform", sa.Text, nullable=False),
    sa.Column("client_version", sa.Text, nullable=False),
    sa.Column("created_at", _ts, nullable=False),
    sa.Column("last_seen_at", _ts, nullable=False),
    sa.Index("ix_devices_account_id", "account_id"),
)

refresh_families = sa.Table(
    "refresh_families", metadata,
    sa.Column("id", _uuid, primary_key=True),
    sa.Column("account_id", _uuid, sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
    sa.Column("device_id", _uuid, sa.ForeignKey("devices.id", ondelete="CASCADE"), nullable=False),
    sa.Column("created_at", _ts, nullable=False),
    sa.Column("absolute_expires_at", _ts, nullable=False),
    sa.Column("sliding_expires_at", _ts, nullable=False),
    sa.Column("revoked_at", _ts),
    sa.Column("revoked_reason", sa.Text),
    sa.UniqueConstraint("device_id", name="uq_refresh_families_device_id"),
    sa.UniqueConstraint("id", "account_id", name="uq_refresh_families_id_account_id"),
    sa.Index("ix_refresh_families_account_id", "account_id"),
)

refresh_tokens = sa.Table(
    "refresh_tokens", metadata,
    sa.Column("id", _uuid, primary_key=True),
    sa.Column("family_id", _uuid, sa.ForeignKey("refresh_families.id", ondelete="CASCADE"), nullable=False),
    sa.Column("generation", sa.Integer, nullable=False),
    sa.Column("token_hash", sa.LargeBinary(32), nullable=False),  # SHA-256
    sa.Column("created_at", _ts, nullable=False),
    sa.Column("rotated_at", _ts),
    sa.Column("rotation_operation_id", _uuid),
    sa.UniqueConstraint("token_hash", name="uq_refresh_tokens_token_hash"),
    sa.UniqueConstraint("family_id", "rotation_operation_id", name="uq_refresh_tokens_family_id_rotation_operation_id"),
    sa.Index("ix_refresh_tokens_family_rotated", "family_id", "rotated_at",
             postgresql_where=sa.text("rotated_at IS NOT NULL")),
)

access_tokens = sa.Table(
    "access_tokens", metadata,
    sa.Column("id", _uuid, primary_key=True),
    sa.Column("token_hash", sa.LargeBinary(32), nullable=False),  # SHA-256
    sa.Column("family_id", _uuid, nullable=False),
    sa.Column("account_id", _uuid, nullable=False),
    sa.Column("issued_at", _ts, nullable=False),
    sa.Column("expires_at", _ts, nullable=False),
    sa.UniqueConstraint("token_hash", name="uq_access_tokens_token_hash"),
    sa.ForeignKeyConstraint(["family_id", "account_id"], ["refresh_families.id", "refresh_families.account_id"],
                            name="fk_access_tokens_family_account", ondelete="CASCADE"),
    sa.Index("ix_access_tokens_family_id", "family_id"),
)
