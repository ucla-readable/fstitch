import java.io.IOException;
import java.io.Writer;
import java.util.Iterator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * An abstract Grouper that implements most of the Grouper functionality.
 */
public abstract class AbstractGrouper implements Grouper
{
	public abstract static class Factory implements GrouperFactory
	{
		protected GrouperFactory subGrouperFactory;
		protected Debugger debugger;

		protected Factory(GrouperFactory subGrouperFactory, Debugger debugger)
		{
			this.subGrouperFactory = subGrouperFactory;
			this.debugger = debugger;
		}
	}


	private Map groupMap; // Map<Object,Grouper>
	private GrouperFactory subGrouperFactory;
	protected Debugger debugger;

	public AbstractGrouper(GrouperFactory subGrouperFactory, Debugger debugger)
	{
		groupMap = new HashMap();
		this.subGrouperFactory = subGrouperFactory;
		this.debugger = debugger;
	}

	public void add(Chdesc c)
	{
		Object groupKey = getGroupKey(c);
		Grouper subgrouper = (Grouper) groupMap.get(groupKey);
		if(subgrouper == null)
		{
			subgrouper = subGrouperFactory.newInstance();
			groupMap.put(groupKey, subgrouper);
		}
		subgrouper.add(c);
	}

	public void render(String clusterPrefix, Writer output) throws IOException
	{
		Set groupEntries = groupMap.entrySet(); // Set<Map.Entry<Object,Grouper>>
		for(Iterator it = groupEntries.iterator(); it.hasNext();) // for(Map.Entry<Object,Grouper> groupEntry : groupEntries)
		{
			Map.Entry groupEntry = (Map.Entry) it.next(); // Map.Entry<Object,Grouper>
			Object groupKey = groupEntry.getKey();
			Grouper subGrouper = (Grouper) groupEntry.getValue();
			renderGroup(groupKey, subGrouper, clusterPrefix, output);
		}
	}

	protected abstract Object getGroupKey(Chdesc c);
	protected abstract void renderGroup(Object groupKey, Grouper subGrouper, String clusterPrefix, Writer output) throws IOException;
}
