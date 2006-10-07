public class ChdescAddAfter extends Opcode
{
	private final int source, target;
	
	public ChdescAddAfter(int source, int target)
	{
		this.source = source;
		this.target = target;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc source = state.lookupChdesc(this.source);
		if(source != null)
		{
			Chdesc target = state.lookupChdesc(this.target);
			if(target == null)
			{
				target = new Chdesc(this.target, state.getOpcodeNumber());
				/* should we really do this? */
				state.addChdesc(target);
			}
			source.addAfter(target);
		}
	}
	
	public String toString()
	{
		return "KDB_CHDESC_ADD_AFTER: source = " + SystemState.hex(source) + ", target = " + SystemState.hex(target);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ADD_AFTER, "KDB_CHDESC_ADD_AFTER", ChdescAddAfter.class);
		factory.addParameter("source", 4);
		factory.addParameter("target", 4);
		return factory;
	}
}
