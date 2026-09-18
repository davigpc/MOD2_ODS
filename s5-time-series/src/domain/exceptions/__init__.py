class ODSBaseException(Exception):
    """Exceção base para todo o sistema ODS."""


class DomainError(ODSBaseException):
    """Erros de violação de regras de negócio do domínio."""


class InvalidWorldCoordinateError(DomainError):
    """Lançado quando uma coordenada de mundo recebe valores não finitos (NaN/inf)."""


class InvalidSpatialCellError(DomainError):
    """Lançado quando uma célula recebe densidade negativa ou não finita."""


class InvalidTimeWindowError(DomainError):
    """Lançado quando start_time é maior ou igual a end_time."""