import java.io.IOException;
import java.io.Writer;

/**
 * A Grouper renders a collection of Chdescs into subgroups.
 */
public interface Grouper
{
	/** Add c to this Grouper */
	void add(Chdesc c);

	/**
	 * Render this groupers Chdescs into subgroups.
	 * clusterPrefix is used as the prefix to all clusters to ensure
	 * cluster name uniqueness.
	 */
	void render(String clusterPrefix, Writer output) throws IOException;
}
