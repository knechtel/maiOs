"""Small Django WSGI application for the maiOs CPython port.

Run from the maiOs shell with: python django &
"""

import sys

# The maiOs port starts with only the packaged standard library on sys.path.
sys.path.insert(0, "/")

from django.conf import settings

settings.configure(
    DEBUG=True,
    SECRET_KEY="maiOs-local-development-key",
    TIME_ZONE="UTC",
    ROOT_URLCONF="__main__",
    ALLOWED_HOSTS=["*"],
    INSTALLED_APPS=[],
    MIDDLEWARE=[],
    TEMPLATES=[],
    # The maiOs image has no system zoneinfo database.
    USE_TZ=False,
)

from django.http import HttpResponse
from django.urls import path


def home(request):
    return HttpResponse("<h1>Django is running on maiOs</h1>")


urlpatterns = [path("", home)]

from django.core.wsgi import get_wsgi_application
from maios_wsgi import serve

application = get_wsgi_application()
serve(application, 8070)
