public class ChdescAddBefore extends Opcode
{
	private final int source, target;
	
	public ChdescAddBefore(int source, int target)
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
			source.addBefore(target);
		}
	}
	
	public String toString()
	{
		return "KDB_CHDESC_ADD_BEFORE: source = " + SystemState.hex(source) + ", target = " + SystemState.hex(target);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ADD_BEFORE, "KDB_CHDESC_ADD_BEFORE", ChdescAddBefore.class);
		factory.addParameter("source", 4);
		factory.addParameter("target", 4);
		return factory;
	}
}
