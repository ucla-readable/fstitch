import java.io.IOException;
import java.io.Writer;

/**
 * A Grouper that groups a Chdesc by the module it is owned by.
 */
public class OwnerGrouper extends AbstractGrouper
{
	public static class Factory extends AbstractGrouper.Factory
	{
		protected String color;
		protected Debugger debugger;
		protected String name;

		public Factory(GrouperFactory subGrouperFactory, String color, Debugger dbg)
		{
			super(subGrouperFactory);
			this.color = color;
			this.debugger = dbg;

			name = "owner[" + color + "]";
			String subName = subGrouperFactory.toString();
			if (!subName.equals(NoneGrouper.Factory.getFactory().toString()))
				name += "-" + subName;
		}

		public Grouper newInstance()
		{
			return new OwnerGrouper(subGrouperFactory, color, debugger);
		}

		public String toString()
		{
			return name;
		}

	}


	protected String color;
	protected Debugger debugger;

	public OwnerGrouper(GrouperFactory subGrouperFactory, String color, Debugger dbg)
	{
		super(subGrouperFactory);
		this.color = color;
		this.debugger = dbg;
	}

	protected Object getGroupKey(Chdesc c)
	{
		return new Integer(c.getOwner());
	}

	protected void renderGroup(Object groupKey, Grouper subGrouper, String clusterPrefix, Writer output) throws IOException
	{
		int owner = ((Integer) groupKey).intValue();
		String clusterName = clusterPrefix + owner;
		if(owner != 0)
		{
			String ownerName = debugger.getState().getBdName(owner);
			if(ownerName == null)
				ownerName = SystemState.hex(owner);
			output.write("subgraph cluster" + clusterName + " {\n");
			output.write("label=\"module " + ownerName + "\";\n");
			output.write("color=" + color + ";\n");
			output.write("labeljust=r;\n");
		}
		subGrouper.render(clusterName, output);
		if(owner != 0)
			output.write("}\n");
	}
}
